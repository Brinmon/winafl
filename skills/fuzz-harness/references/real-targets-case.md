# 实战案例库：四个真实目标 + 32位探针（2026-10-17 验收）

> 配套 `SKILL.md`。同日批量建 5 个 harness、fuzzkit 端到端跑通 4 个 campaign。
> 判据口径见 SKILL 第 3 步（stability ≥90%，与 bitmap_cvg 同看）。

## 验收总表（fuzzkit start --duration 10m，实测 545s，四 campaign 并行）

| harness | 目标（DLL/位数） | stability | var_paths | eps | execs | paths | bitmap | crash |
|---|---|---|---|---|---|---|---|---|
| sevenzip_7z | 7-Zip 7z.dll x64（COM，60格式） | **100.00%** | 0 | 158 | 86163 | 152 | 9.94% | **1**（FINDING-1） |
| avformat_x64 | Wireshark avformat-61+avcodec-61 x64 | **100.00%** | 0 | 344 | 187880 | 52 | 10.06% | 0 |
| webp_image | WeGame libwebp.dll x64 | **98.96%** | 1 | 713 | 388983 | 115 | 2.92% | 0 |
| libcrypto_x64 | Intel libcrypto-3-x64 (OpenSSL 3) | 81.79%→**99.84%**（iterations=1 冷进程；见案例 3） | 180→11 字节 | 776→12 eps | 422266（热） | 284 | 11.14% | 0 |
| win32_probe（32位） | SZPlayer libcrypto-1_1 **x86** | **100.00%** | 0 | 1211 | 662281 | 227 | 5.49% | 0 |

- **32 位**：build64 afl-fuzz 原生 fuzz x86 目标（TinyInst README Note#1 +
  `debugger.cpp GetProcessPlatform` 证据），harness 用 `vcvars32` 编即可，零二次开发。
- 并行四跑 eps 会相互摊薄（单跑参考 win32_probe 1211 eps）；stability 不受影响。

## 每案例要点（坑 + 契约）

### 1. sevenzip_7z（COM 形态代表）
- 契约来源：7-Zip 源码 `DllExports2.cpp`/`ArchiveExports.cpp`/`IArchive.h`/`IDecl.h`。
  `IID = 23170F69-40C1-278A-0000-000<gid>00<sid>0000`；格式枚举 =
  `GetNumberOfFormats` + `GetHandlerProperty2(i, kClassID=1)`(BSTR 16B) + `GetIsArc` 预筛。
- **坑 A：stream 生命周期**——handler 在 Open..Close 期间持有 stream（Extract 还要读）；
  Open 后立刻 Release = UAF 段错误（实测）。Close 后再放。
- **坑 B：S_FALSE 门**——7z `Open` 返回 `S_FALSE(1)` = "不是本格式"，但 `SUCCEEDED(1)==true`；
  半初始化 handler 上调 GetProperty/Extract = NULL 虚调用崩溃（实测 rcx=0）。
  **只认 S_OK**：`if (hr != S_OK) hr = E_FAIL;`
- **坑 C**：C++ 手写 COM 不能调 `void*` vtable → 类型化 vtable 结构体；
  `IsEqualGUID` C++ 重载吃引用不吃指针 → `memcmp` 自封装；`PropVariantClear` 在 **ole32.lib**。
- **坑 D**：`extern "C"` 只能写在 `.cpp`；`.c` 文件里是语法错误（x64 C 符号本就无修饰）。
- Extract 用 testMode=1 + 丢弃流 + 8MB 上限（防解压炸弹）；data error 吞掉继续。

### 2. avformat_x64（多模块 + 库所有权形态代表）
- **零 struct 访问**：把 `@@` 文件路径当 URL 直接给 `avformat_open_input(&ctx,path,...)`，
  失败由库回收（`demux.c` fail 路径已确认）、成功 `avformat_close_input` 全权回收——
  不碰 AVFormatContext 字段 = 不依赖 struct 版本布局、零跨堆。
- **不调 `avformat_find_stream_info`**：它开解码器（frame threading 默认 cpu 数线程）= stability 杀手；
  流解析（avcodec parser，单线程）在我们自己的 `av_read_frame` 循环里照样发生 → 100% stability。
- `protocol_whitelist=file,crypto` 防垃圾输入撞上 hls/dash 触发网络挂起；
  `av_log_set_level(-8)` 压 stderr（否则拖垮 eps）。
- 多模块：`modules = ['avformat-61.dll','avcodec-61.dll']`（TinyInst `GetOptionAll` 支持重复 flag）。

### 3. libcrypto_x64（分诊闭环样本 —— 根因已锤死）

- 首跑 81.79%/180 var → 预热补全全部 6 种子类无效（81.98%）→ 推翻假设重查。
- **诊断链**（应用 `attribution/patches/diag-and-clear.patch` 手工 hunk → 诊断版 afl-fuzz）：
  - var_bytes=1314；模式分布 **new0=856（首现分支后消失）+ bothNZ=422（重试次数掉档，
    如 old=59→24、old=91→1、old=189→188±1）+ old0=38**；var_byte 索引逐输入散布；
  - litecov 同输入×6 迭代 = 逐字节稳定、showmap 三跑零 diff、**单跑与并行同为 81.8%**
    → 排除输入自身非纯、排除争用抖动；
  - **根因 = OpenSSL3 EVP/store 取数缓存**：发现执行=cache miss（慢路径+miss 分支）、
    紧接的校准执行=hit → 该队列条目永久标 variable；特征空间（OID/算法/扩展组合）
    随变异近似无界 → **预热追不完（skill step4 的活例）**。
- **修法定案：`-iterations 1`（每例冷进程）** —— 发现/校准两侧同冷，
  **81.75% → 99.84%**（var_bytes 1314→11）。代价：eps 776→~12
  （单例下限 16.6ms≈60 eps，TinyInst 每次冷启动重翻译 1.8MB 模块 ≈+70ms = 主成本）。
  两 profile 均可跑：`fuzzkit.toml` 默认 iterations=2000（速度优先 81.8%）、
  本目标定稿 iterations=1（稳定性优先 99.8%）；再往上才轮到 fork-server（克隆保留翻译态）。

### 4. webp_image（依赖链形态代表）
- `LoadLibrary` 全路径 ≠ 依赖可解析：libwebp 依赖同目录 `libsharpyuv.dll` →
  **`LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)`**（错误 126 的根因）。
- 目录名含非 ASCII（英雄联盟）→ 运行时 `FindFirstFile` 枚举一级子目录拼路径（源码不写中文）；
  **FindFirstFile 通配符只允许文件名段**，不能穿目录层（`*\Cross\...` 无效）。
- 输出缓冲由 harness 提供（`WebPDecodeRGBAInto`）→ 无跨 DLL 堆所有权问题。

### 5. win32_probe（x86 形态代表）
- vcvars32 编 harness；afl-fuzz/litecov/showmap 全部继续用 build64。
- `-y` 模式 showmap 支持（`afl-showmap.c` optstring 含 y）；预检干跑直接可用。

## FINDING-1：7z.dll 模糊测试发现 1 个 crash（存证 + 已完成的归因边界）

- 产物：`harnesses/sevenzip_7z/runs/epoch1-20261017-092318/engine/crashes/id_000000_00_...`
  （2288B，变异自 a.7z；另 status 显示 crash 累计 2，以 crashes 目录为准）。
- **复现矩阵**：独立进程 10/10 exit0（静默）；litecov 插桩下 **3/3-6/6 必崩**。
- 故障点：ntdll 堆函数（地址 0x7FFBF8Exxx-F8F2xxx 漂移 = 堆破坏延迟触发，
  7z.dll 走 msvcrt→默认进程堆，与 harness ucrt 同堆交织）。
- **二分（harness 内置 env 门禁）**：OPEN_ONLY / NO_EXTRACT / 单格式 / 区间
  [0,47] / [48,59] 各自 0 崩，全量 6 崩 → **跨段交互**（某格式 Open 破坏 + 后续分配触发）。
- 待办（需管理员）：`gflags /p /full harness.exe` 页堆抓写越界现场 → omida 定位函数。
  复现协议：`litecov -instrument_module 7z.dll -target_method fuzz_one -iterations 3 -- harness.exe <crash>`。
- 门禁 env（已内建 harness，平时不影响 fuzz）：`HARNESS_VERBOSE` `HARNESS_NO_EXTRACT`
  `HARNESS_OPEN_ONLY` `HARNESS_FMT` `HARNESS_FMT_LO/HI`。

## fuzzkit 端到端实录（4 campaign + 复验）

- 流程：`fuzzkit build <h>`（跑 build.cmd + 13 项预检含 afl-showmap 干跑）→
  `fuzzkit start <h> --duration 10m`（detached supervisor）→ `status`/`events` → `stop`。
- **实测发现（给 fuzzkit 的反馈）**：
  1. `leftovers` 残留检查会把**同 workspace 第一个 campaign 的 afl-fuzz 误判为残留** →
     并发启动被拒；并发场景对后续实例加 `--skip-preflight`（build 时已全检）。
  2. 重编 harness 后 `start` 被 `target_hash` 漂移检测拒绝（ADR-0016，**设计正确**）→
     用 `--fresh` 开新 epoch。
  3. 模块 sha256 pin（ADR-0011）在 build 预检即校验；fuzz.toml 单引号字符串避免
     Windows 路径反斜杠转义。
  4. 观察（待确认）：`status` 的 crash 计数列出现 2 而 crashes 目录实存 1 文件
     （sevenzip_7z epoch1，fuzzer_stats unique_crashes=1）——疑似 stop 冲刷双计。
  5. 重建 afl-fuzz 后残留旧进程会阻塞 `build`（leftovers 正确拦截此场景 ✓）——
     手工 kill 用 bash `taskkill //IM`（git-bash 会把 `/IM` 当路径吃掉）。
- `build` 预检唯一 warn = 非管理员（WinAFL 部分场景需管理员，本工作流 TinyInst 不受影响）。

## 构建/启动的 cmd 环境坑（全部实踩）

| 坑 | 现象 | 对策 |
|---|---|---|
| GBK cmd 读 UTF-8 中文注释 | .cmd 报乱码/误执行 | .cmd 注释**只写 ASCII** |
| `cmd //c "嵌套引号"` | 引号被吃、命令不识别 | 启动命令写进 .cmd 脚本再 `cmd //c xxx.cmd` |
| `artifacts\` 不存在 | LNK1164/1104 | build.cmd 开头 `if not exist artifacts mkdir` |
| exe 被运行中的 campaign 锁定 | LNK1168 | 重建前 `fuzzkit stop`（或 taskkill afl-fuzz） |
| python 文本模式写源文件 | LF→CRLF 漂移毁掉字符串锚 | 补丁脚本统一 `newline=''` 读写 |
| 双层转义吃掉 `\n` | 锚匹配失败、TRACE 格式串断裂 | 行级处理 + `chr(92)` 构造反斜杠 |
