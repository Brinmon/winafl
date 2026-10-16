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
| 1 | **预热（最关键）** | 进入 target function **之前**，用**三类代表性样本**各解析一次：①有效输入 ②完全不匹配的垃圾字节 ③**magic 匹配但内容损坏**的样本。三类覆盖三种"首现路径"；每换一个目标库，重新枚举它自己的 one-shot 路径（GType/class_init、解码器注册、首次失败分支……），必要时补第 4、5 类样本。预热样本文件须随 harness 一起分发（放对 CWD）。 |
| 2 | **关缓存/池** | `GetProcAddress` 找库的缓存关闭 API（libvips: `vips_cache_set_max(0)`；对应你的库找等价物：对象池、LRU、slice 等）。缓存容量边界穿越校准窗口 = 经典污染源。 |
| 3 | **单线程** | `vips_concurrency_set(1)` / 线程池 API 显式压 1；否则线程调度会让 BB 命中计数逐次漂移。 |
| 4 | **每轮回收** | 每次调用后 unref 结果对象、清错误缓冲、关句柄。persistent 循环里任何累积（泄漏 ~20KB/次）都是定时炸弹。 |

## 第 2 步：构建规范

- `__declspec(noinline)` 标注 target function；**带 PDB 构建**（`-target_method` 靠符号定位）
- 输入读取用**动态缓冲**（模板已改掉 64KB 静态截断的老毛病）
- 构建命令见 `scripts/build_harness.cmd`（vcvars64 + `/O2 /MD /Zi` + `/DEBUG:FULL`）

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
4. 若首现路径**无穷多**（格式空间无限、每种都一套一次性初始化）→ 预热表追不完 → 升级到
   **fork 路线**（每例干净进程；`E:\GitCode\winafl\fork-server` 已有 RtlCloneUserProcess 引擎，
   10000 次克隆实测 0 泄漏）或非 persistent 模式（每例重启）

## 铁律

- **不要相信玩具目标的 100%**——玩具 warmup 会把一次性路径全跑完，真实深调用点才会暴露问题
- stability 必须与 `bitmap_cvg` **同时**看，且注意口径是否含一次性死重量
- 诊断/案发定位的完整方法论（逐层隔离 L1–L8）：`references/libvips-case.md`
