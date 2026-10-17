---
name: fuzz-harness
description: 为闭源 Windows DLL/程序创建并验收 WinAFL harness（persistent 模式，TinyInst/DynamoRIO/Intel PT）。当用户要为新 fuzz 目标写 harness .c、要把某个 DLL 接入 fuzz、问 harness 怎么写才稳定、或问新目标如何开始 fuzz 时使用。包含四件套状态隔离清单、构建规范（noinline+PDB）、10 分钟 stability 验收协议与案发分诊流程。
---

# WinAFL Harness 工厂（闭源 Windows 目标）

## 核心原理（为什么 harness 必须这么写）

AFL 的 stability = 同一输入重复执行时覆盖位图的一致率。`calibrate_case` 把**第一次执行**
（`first_trace`）当基线。持久化循环里只要目标有**"每进程只跑一次"的代码路径**（lazy init /
class_init / 缓存池 / 线程时序），基线就与后续执行不一致 → stability 崩塌（实测 16.72%），
且漂移的字节会被灌进 virgin map 造成 `bitmap_cvg` 虚高。

**结论：harness 的职责 = 让目标函数体在进程内变成纯函数。** 完整证据链见
`references/libvips-case.md`（16.72% → 98.72% 的归因与干预全记录）。

## 第 1 步：按模板写 harness

用 `assets/harness_template.c` 起手，必须落实**四件套**：

| # | 项 | 要点 |
|---|---|---|
| 1 | **预热（最关键）** | 进入 target function **之前**，用**三类代表性样本**各解析一次：①有效输入 ②完全不匹配的垃圾字节 ③**magic 匹配但内容损坏**的样本。三类覆盖三种"首现路径"；每换一个目标库，重新枚举它自己的 one-shot 路径（GType/class_init、解码器注册、首次失败分支……），必要时补第 4、5 类样本。**预热表按"种子类"枚举，不按样本个数**——多格式目标（7z 60 格式）或多对象型库（OpenSSL3 按对象类型/算法首次使用惰性加载）要把全部种子类预热一遍（libcrypto 实测 3 类预热=81.79%，全类预热后复验）。预热样本文件须随 harness 一起分发（放对 CWD）。 |
| 2 | **关缓存/池** | `GetProcAddress` 找库的缓存关闭 API（libvips: `vips_cache_set_max(0)`；对应你的库找等价物：对象池、LRU、slice 等）。缓存容量边界穿越校准窗口 = 经典污染源。 |
| 3 | **单线程** | `vips_concurrency_set(1)` / 线程池 API 显式压 1；否则线程调度会让 BB 命中计数逐次漂移。 |
| 4 | **每轮回收** | 每次调用后 unref 结果对象、清错误缓冲、关句柄。persistent 循环里任何累积（泄漏 ~20KB/次）都是定时炸弹。 |

## 第 2 步：构建规范

- `__declspec(noinline)` 标注 target function；**带 PDB 构建**（`-target_method` 靠符号定位）
- 输入读取用**动态缓冲**（模板已改掉 64KB 静态截断的老毛病）
- 构建命令见 `scripts/build_harness.cmd <src> <out.exe> [x64|x86]`（vcvars + `/O2 /MD /Zi` + `/DEBUG:FULL` + oleaut32/ole32）
- **位数**：x64 目标用默认；**x86 目标 harness 用 `x86` 参数（vcvars32）**——build64 的
  afl-fuzz/litecov/showmap 原生可插桩 WOW64 目标（TinyInst README Note#1、
  `debugger.cpp GetProcessPlatform`），无需 32 位引擎；进程位数由 harness 决定
- **`.c` 里不写 `extern "C"`**（语法错误；x64 C 符号无修饰）；C++ harness 的 `fuzz_one`
  必须 `extern "C"`；C++ 手写 COM 用**类型化 vtable 结构体**（不能调 `void*`），
  GUID 比较用 memcmp（`IsEqualGUID` C++ 重载吃引用），`PropVariantClear` 在 ole32.lib
- **`.cmd` 注释只写 ASCII**（GBK cmd 误读 UTF-8 中文）；嵌套引号的启动命令写进 .cmd
  再 `cmd //c`；build.cmd 开头 `if not exist artifacts mkdir`；**重建前先 `fuzzkit stop`**
  （运行中的 campaign 锁 exe → LNK1168）

## 第 3 步：10 分钟验收协议（新目标必须跑）

```bat
afl-fuzz.exe -i <seeds> -o out_check -t 20000 -I 15000 -y -- ^
  -instrument_module <目标.dll> -target_module <harness>.exe -target_method fuzz_one -nargs 1 ^
  -iterations 2000 -persist -loop -- <harness>.exe @@
:: 10 分钟后：
findstr /C:"stability" /C:"variable_paths" /C:"execs_per_sec" /C:"bitmap_cvg" out_check\fuzzer_stats
```

**判据**：
- `stability ≥ 90%`（工程黄线 95%）—— 官方口径 >90% "usually fine"，**不必追 100%**
- `variable_paths` = 0 或个位数
- `execs_per_sec ≥ 预期基线 0.5×`
- `bitmap_cvg` 用**修复后的诚实口径**对比（一次性路径不再计入；拿旧值比会误判"覆盖下降"）

## 第 4 步：不达标时的分诊（按序）

1. 看 `variable_paths` 与案发输入名（诊断版 fuzzer 输出 `var_bytes.txt` / `vardiff_*.txt`；
   上游版为空属正常——该 fork 的 `mark_as_variable` 落盘被砍）
2. 最常见根因 = **还有一类"每进程首现路径"没预热** → 把案发输入对应的样本类型加入预热，重跑验收
3. 次常见 = 缓存/线程/泄漏没关干净 → 回查四件套
4. 若首现路径**无穷多**（格式空间无限、每种都一套一次性初始化）→ 预热表追不完 → 按序升级：
   (a) **`-iterations 1` 每例冷进程**（最小改动；发现/校准两侧同冷，缓存/懒加载型根因必治）——
       OpenSSL3 实测 81.75%→99.84%，代价 eps ×1/60（TinyInst 每次冷启动重翻译目标模块，
       1.8MB ≈ +70ms；单例下限公式：实测 floor≈16ms→~60 eps 理论上限）
   (b) **fork 路线**（每例干净进程且克隆保留插桩态 → 克隆开销≈4ms，比 iterations=1 快一个量级；
       `E:\GitCode\winafl\fork-server` 已有 RtlCloneUserProcess 引擎，10000 次克隆实测 0 泄漏）
   (c) 非 persistent 模式（每例重启）
   非 persistent/冷进程的前提同样是先排除预热可覆盖的有界 one-shot（四件套 #1）
5. **复现矩阵先定性**：独立进程不崩 + litecov 插桩下必崩 = 崩溃**依赖插桩上下文**，
   此时**不能**直接断言“真目标 bug”（本案早期误判“潜伏堆破坏”，终局为 TinyInst 假象 → 必须过 #7 闸门）；
   用 harness 内置 env 门禁二分（`HARNESS_NO_EXTRACT` / `HARNESS_OPEN_ONLY` /
   `HARNESS_FMT_LO..HI` 单格式区间）压到阶段/格式段做定位；要断言真 bug 还需
   `gflags /p /full` 页堆（需管理员）+ omida 定位。完整实录见 `references/real-targets-case.md` FINDING-1
6. **"变量字节」三步定性法**（实测有效，配诊断补丁）：
   ① showmap 同输入×3 diff = 0 → 输入是纯函数；② litecov 同输入×6 "new offsets" ≈ 0
   → 进程内无残留 one-shot；③ 仍高 → 应用 `attribution/patches/diag-and-clear.patch`
   （手工 hunk，36 行）→ 看 var_bytes 模式：**new0 居多=首现分支、bothNZ 掉档=缓存/重试计数**
   → 按 (a)/(b)/(c) 选升级路。诊断版用完必须 `git checkout` 恢复并重建（引擎保持上游态）
7. **crash 三步验证闸门（对外宣称“发现”前必过）**——实测案例：7z 的 1 个 crash 经此判定为
   TinyInst 假象而非 7-Zip 漏洞（六条件证据矩阵见 `references/real-targets-case.md` FINDING-1）：
   ① **独立进程重放 ×10**（不插桩）；② **第二引擎对照**（DynamoRIO：`afl-showmap -D <drrun目录>`
   —— 注意 -D 是目录非 drrun.exe，`-c winafl.dll` 按 CWD 解析 → 先把 winafl.dll 拷进 harness 目录；
   路径用相对路径避免 POSIX/Windows 混用）；③ **原生高压循环**（harness 内建
   `HARNESS_LOOP=N` 同进程 N 轮 writer+tripper）。三者全干净 + 只在目标插桩下必崩
   → 判**工具链假象**（仍有价值：提上游 issue + 假 crash 会污染 unique_crashes，长跑前应先验）；
   任一独立路径复现 → 才升级为“真目标 bug”，再上页堆/反编译定位。
   另：VEH 抓不到 litecov 子进程的 AV（first-chance 被调试器直杀，进程内只能看到
   加载器断点 0x80000003 噪声）——别指望 in-process dump 取栈。
8. **形态契约坑速查**（详见 `references/real-targets-case.md`）：COM 目标要守住库的
   对象生命周期（7z stream 活到 Close）与返回值语义（7z Open 的 S_FALSE=拒绝，**只认 S_OK**，
   半初始化对象上续调 = NULL 虚调用）；带依赖链的 DLL 用
   `LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)`；"每轮回收"含库的错误队列
   （OpenSSL `ERR_clear_error`）与 PROPVARIANT

## 铁律

- **不要相信玩具目标的 100%**——玩具 warmup 会把一次性路径全跑完，真实深调用点才会暴露问题
- stability 必须与 `bitmap_cvg` **同时**看，且注意口径是否含一次性死重量
- 诊断/案发定位的完整方法论（逐层隔离 L1–L8）：`references/libvips-case.md`
- 多目标实战库（四案例验收表、每形态契约坑、fuzzkit 端到端实录、cmd 环境坑）：
  `references/real-targets-case.md`

## 第 5 步：fuzzkit 对接（campaign 管理，验收通过后接入）

fuzz.toml（L3，`deny_unknown_fields`，路径用单引号字面串）核心字段：

```toml
[fuzz]
name = 'sevenzip_7z'
[target]
exe = 'artifacts/harness.exe'
args = ['@@']
arch = 'x86_64'
[[target.modules]]          # 模块 sha256 pin（ADR-0011），build 预检即校验
name = '7z.dll'
path = 'E:\OtherTool\7-Zip\7z.dll'
sha256 = '8820...'
[instrumentation]
mode = 'tinyinst'
modules = ['7z.dll']        # 多模块直接加
iterations = 2000           # 与 10 分钟验收协议一致
[seeds]
dirs = ['seeds']
```

流程：`fuzzkit build <h>`（build.cmd + 13 项预检，**含 afl-showmap 干跑验证插桩**）→
`fuzzkit start <h> --duration 10m`（detached；`status`/`events` 跟踪；`stop` 收网）→
`crashes` 回收。实测三规则：

1. 同 workspace **并发启动**时，后续实例会被 `leftovers` 检查误拦（把首个 campaign 的
   afl-fuzz 当残留）→ 后续加 `--skip-preflight`（build 时已全检）
2. 重编 harness 后 start 被 `target_hash` 漂移拒绝（**设计正确**，ADR-0016）→ `--fresh` 新 epoch
3. 预检 `environment` warn（非管理员）对 TinyInst 模式不影响，可继续
