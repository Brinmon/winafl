# 调研报告：闭源 Windows DLL fuzzing 的 stability 问题与可行技术路线

> 日期：2026-10-16 · 承接 `RESEARCH-PROMPT.md` / `HANDOVER.md`
> 方法：官方文档 + fuzzer 源码 + 论文 + 本地仓库静态审阅（**未重复任何已做过的 fuzzing 实验**）
> 环境：Win10 19045 (22H2) / x64 / VS2022 · 目标：QQ `libvips-42.dll`（20MB 闭源 x64）
>
> 📌 **2026-10-16 实战归因更新（后验修正，详见 `attribution/ATTRIBUTION.md`）**：
> 根因已实战定位并修复——**libvips 首次调用 lazy init 被 AFL 校准计入 first_trace（主因，98% 不稳字节）
> + operation cache 容量边界 + 默认多线程**；WinAFL 计算/插桩引擎/AFL 核心均无 bug。
> 修复（harness 三件套：预热解析 + `vips_cache_set_max(0)` + `vips_concurrency_set(1)`）后
> **stability 16.72% → 99.01%** → **B2 胜出，不需要 fork（B3 搁置）**。
> ⚠️ 判据修正：`bitmap_cvg ≥12.73%` 作废（旧值被一次性 lazy init 虚高，诚实口径 ≈2.6%）。

---

## 0. 执行摘要：六个必答问题的直接回答

| # | 问题 | 回答 |
|---|---|---|
| 1 | stability 16.72% 是否可接受？ | **否**。低于 AFL++ 官方背书的"above 20%"下限，处于官方"below 80–90% 应采取对策"区间；且我们处于官方点名警告的 **persistent + 不稳定** 组合（crash 可能无法重放）。但它不是"零信号"——bitmap_cvg=12.73% 说明覆盖收集本身正常，坏的是一致性。 |
| 2 | stability 100% 是否是正确目标？ | **方向对，数值要修正**。AFL++ 明确"100% 是 best case"，但也明确"90% 通常没问题、80% 也可以"。建议验收目标改为：**stability ≥ 90%（工程黄线 95%）+ crash 100% 可重放 + bitmap_cvg 不下降**。把"字面 100%"当硬判据会为最后几个百分点付出不成比例的代价。 |
| 3 | 正确技术路线（B1–B5）？ | **B5（分阶段）**：先跑 1–2 天低成本实验验证 **B2**（`VIPS_CONCURRENCY=1` + 关闭 operation cache + 修 harness 缺陷——OSS-Fuzz 官方 libvips harness 正是这么做的）；**架构上的长期正解是 B3 的修正版**——fork（P2 已完成）+ **"插桩一次、快照复用"的覆盖率**（首选 Winnie 式 in-process int3 fullspeed，备选 peafl64 静态改写），**不是**继续 DR/TinyInst。B1 只作 1 小时对照实验；**B4 否决**。 |
| 4 | 下一步做什么？ | §6 的 E0–E6 实验（命令级，1–2 天内可判读 B2 vs B3）。 |
| 5 | 哪些结论证据不足？ | §8 不确定性清单（9 条，含 E3 的 ASLR 口径、WinFuzz x64 工作量、ZAFL 平台等）。 |

**对交接文档最重要的一条修正**：A1 的结论"fork 与懒翻译在原理上互斥"**只否定了 DynamoRIO/TinyInst 这类外部插桩引擎**，并不意味着"fork 与覆盖率互斥"。WINNIE（NDSS'21）已经发表并开源了 **fork + 覆盖率共存** 的完整实现：把断点处理器放进**进程内**（VEH/UEF 随 CoW 快照一起被克隆），断点在 fork 前一次性打上、子进程继承。**P3 值得继续，但覆盖率来源必须换。**

---

## 1. 问题 1（最高优先级）：stability 到底影响什么？必须追求 100% 吗？

### 1.1 结论（对 5 个子问题的明确回答）

**（1）低 stability 的具体后果（有官方出处）**

| 后果 | 依据 |
|---|---|
| **路径识别困难（漏报新路径）**：不稳定边使 AFL++ 难以判断哪些是真正的"新路径" | AFL++ FAQ："*The more 'unstable' edges there are, the harder it is for AFL++ to identify valid new paths.*" |
| **崩溃无法重放**（persistent 模式特有，官方点名） | AFL++ FAQ："*a large number of unstable edges can mean that the target keeps internal state and therefore it is possible that crashes cannot be replayed*" |
| **假阳性崩溃 + 漏报崩溃**（污染的堆/全局状态导致 spurious & missed crashes） | WinFuzz (USENIX'23) 摘要："*in-memory looping … does not reset process state between test cases—overwhelming fuzzers with false-positive crashes caused by polluted heap and global state*"；"*Starting from an impossible program state leads to both missed crashes … and spurious crashes*" |
| **覆盖率的部分丢失**（每条不稳定边 = 该边的"部分覆盖损失"） | AFL++ best_practices："*With instability, you basically have a partial coverage loss on an edge, with ignored functions you have a full loss on that edges.*" |
| **队列噪声**：不稳定边反复产生"伪新路径"信号，挤占 fuzzing 时间 | 由上两条推论；AFL 2.43b 源码把同一输入校准期内 checksum 不一致的字节记入 `var_bytes`（`afl-fuzz.c:3096-3097`），这些边每次校准都可能触发 `has_new_bits` |
| **资源泄漏/超时/进程被杀**（persistent 状态污染的伴随症状，非 stability 指标本身但同源） | WINNIE 论文 §III-B："*even the smallest side-effects will gradually accumulate over time, finally leading to problems like memory leaks, unreproducible crashes and hangs, and unreliable coverage. We experienced such errors very often in practice*" |

**（2）16.72% 在实际使用中是否"可用"？**

- 官方原文是 *"even a value above 20% can still result in successful finds of bugs"* —— 这是**下限表述**，不是对 20% 以下的背书。**16.72% 低于官方给出的唯一一个"仍可能成功"的数字**。
- 官方同时给出行动要求：*"it is recommended that for values below 90% or 80% you should take countermeasures to improve stability."* → 16.72% 处于"必须采取对策"区间，不是"接受现实"区间。
- 更关键的定性差别：FAQ 的 20% 说法是**一般性**的；而针对 **persistent 模式 + 不稳定**，官方另有更严厉的专门条款（crash 不可重放、建议去掉 `AFL_LOOP` 或用 `@@` 非持久方式）。我们恰好同时命中这两个条件。
- **未找到任何"stability < 20% 仍有效挖 bug"的公开案例/实测**（见 §8 不确定性 #1）。
- 反面证据：WINNIE 评估中 persistent 模式的三个 harness "几乎立刻"出现内存/句柄泄漏；WinFuzz 论文把 in-memory looping 列为产生 spurious/missed crashes 的机制。
- **判定：16.72% 不可接受为长期状态；但也无需恐慌——它是"可诊断、可修复"的状态，不是引擎报废。**

**（3）"100%" 是否是合理目标？**

- AFL++：*"For fuzzing, a 100% stable target that covers all edges is the best case."* —— 100% 是**理想值**。
- AFL++：*"A value above 90% is usually fine and a value above 80% is also still ok."* —— **90% 是实际达标线**。
- 业界实测参考：AFL++ Discussion #2082（libopenmpt 长期 fuzzing）：`__AFL_INIT` 下 perfect stability，`__AFL_LOOP(10000)` 下 **99.xx%** —— 即"做得好的 persistent 目标"的现实水位是 99%+，不是非黑即白。
- 多线程真实目标：AFL 用户社区共识（afl-users 线程 "AFL/AFL++ and Multithreaded Programs"）是"stability 会降但仍可 fuzz"；AFL 自带文档把多线程列为 variable behavior 的**四大常见原因之一**（`afl_docs/status_screen.txt:297-315`），并称对复杂多线程程序可用 `AFL_NO_VAR_CHECK=1` 跳过（`afl_docs/env_variables.txt:108`）——即官方认为**多线程目标业界普遍达不到 100%，这是常态而非故障**。
- 对闭源 Windows persistent 目标，WINNIE/WinFuzz 的立场是：**发散不可避免，应当换执行机制（fork/快照）而不是追求调到 100%**（README 原文见 §2.1.7）。
- **判定：把 100% 作为"方向"保留，把 ≥90%（工程验收 95%）作为硬判据。**

**（4）stability 与 crash 可复现性的定量关系**

- **明确结论：不存在公开的定量研究**（stability 百分比 → 重放成功率的函数关系无人发表过；见 §8 不确定性 #1）。任何声称"16% stability ⇒ X% 可重放"的说法都没有依据。
- 定性机制链条是权威且明确的：
  1. persistent 下状态随迭代累积（AFL++ FAQ、WINNIE §III-B、WinFuzz 摘要三家一致）；
  2. 崩溃发生在**依赖累积状态的执行**上 → 用单输入、干净进程重放时状态不同 → 重放失败（AFL++ FAQ 原文）；
  3. 反向也成立（漏报）：WinFuzz："*Starting from an impossible program state leads to … missed crashes*"。
- **可操作的缓解手段（官方）**：非 persistent 重放（`@@` 每例新进程）；AFL++ `AFL_PERSISTENT_RECORD`（`config.h:137-147`，把 crash 前 N 个输入一并落盘为 `RECORD:...`，配合 `AFL_PERSISTENT_REPLAY` 重放）；降低 `AFL_LOOP` 值（FAQ 建议 e.g. 100）。**注意：这三项 WinAFL/AFL 2.43b 基座都没有**——WinAFL 侧要靠"每例干净状态"（fork 或非 persistent）实现同等保证。
- **fork 模式天然给出可重放性**：每个测试用例在干净克隆中执行 → crash 出现在干净态 → 同一输入重放必然复现（这正是 BASELINE.md "crash 可复现性 100%" 判据的理论依据，也是 WINNIE 论文"*fork … containing any side-effects, like crashes or hangs*"的论点）。

**（5）AFL 2.43b vs AFL++ 的 stability 计算差异与 `var_bytes` 含义**

- **公式两代相同**（已核对双方源码）：
  - AFL 2.43b（WinAFL 基座）：`stab_ratio = 100 - var_byte_count * 100 / t_bytes`（`afl-fuzz.c:4525`）；红色阈值 `<85% 且 var_byte_count>40`（`afl-fuzz.c:4847-4848`）。
  - AFL++ stable：`stab_ratio = 100 - (((double)afl->var_byte_count * 100) / t_bytes)`（`src/afl-fuzz-stats.c:937`）；UI 阈值逐字相同（`src/afl-fuzz-ui.c:1466-1470`）。
- **`var_bytes` 语义**：位图中"在**校准（calibrate_case）**同一输入的重复执行之间出现过值变化"的字节。AFL 2.43b：`first_trace[i] != trace_bits[i]` 时置位（`afl-fuzz.c:3096-3097`），`var_byte_count = count_bytes(var_bytes)`（`:3149`）。`t_bytes = count_non_255_bytes(virgin_bits)` = 被触达过的位图字节数。即 **stability = "重复执行同一输入时位图保持一致的比例"**。
- **关键口径**（两代相同，对本项目很重要）：stability **只在校准阶段采样**（新 queue 项 + 变量行为重校准），不是持续统计。persistent 模式下，校准的多次执行发生在**同一个已累积状态的进程里**——所以它测的正是"状态污染"。
- **AFL++ 相对 2.43b 的实际差异**（都对本项目有用）：
  1. `fuzzer_stats` 额外输出 `var_byte_count` 和 **`var_bytes` 的逐字节索引列表**（`src/afl-fuzz-stats.c:511,613-616`），配合 `AFL_DEBUG=1` 可直接定位不稳定边 ID（best_practices §2 步骤）；
  2. 校准中检测到不稳定时打印 `instability detected during calibration: <file>` 警告（`src/afl-fuzz-run.c:547-557`，`AFL_NO_WARN_INSTABILITY` 可关）；
  3. 新增 `AFL_PERSISTENT_RECORD`/`AFL_PERSISTENT_REPLAY` 崩溃记录-重放机制；
  4. 两代都有 `AFL_NO_VAR_CHECK`（跳过 variable 检测）。
  → **不存在"换新版 AFL++ 会让 16.72% 变高"的算法红利；数字口径是可比的。**

### 1.2 问题 1 证据表

| 结论 | 来源类型 | 原文引用（节选） | 链接 |
|---|---|---|---|
| stability 定义与后果 | 官方文档 | "*Stability is measured by how many percent of the edges… The more 'unstable' edges there are, the harder it is for AFL++ to identify valid new paths.*" | https://aflplus.plus/docs/faq/ |
| persistent + 不稳定 → crash 不可重放 | 官方文档 | "*…a large number of unstable edges can mean that the target keeps internal state and therefore it is possible that crashes cannot be replayed. In such a case do either **not** fuzz in persistent mode… or set a low `AFL_LOOP` value, e.g. 100, and enable `AFL_PERSISTENT_RECORD`*" | https://aflplus.plus/docs/faq/ |
| 数值水位 | 官方文档 | "*A value above 90% is usually fine and a value above 80% is also still ok, and even a value above 20% can still result in successful finds of bugs. However, it is recommended that for values below 90% or 80% you should take countermeasures*" | https://aflplus.plus/docs/faq/ |
| 100% vs 90% 的取舍 | 官方文档 | "*a 100% stable target that covers all edges is the best case. A 90% stable target that covers all edges is, however, better than a 100% stable target that ignores 10% of the edges.*" | https://aflplus.plus/docs/best_practices/ |
| persistent 不稳定且非 persistent 正常 → 弃用 persistent | 官方文档 | "*If a persistent target is unstable whereas when run non-persistent is fine, then this means that the target is keeping internal state… Fuzz such targets **without** persistent mode.*" | https://aflplus.plus/docs/best_practices/ |
| persistent 状态污染 → 泄漏/不可重放/不可靠覆盖 | 论文 (NDSS'21) | "*…even the smallest side-effects will gradually accumulate over time, finally leading to problems like memory leaks, unreproducible crashes and hangs, and unreliable coverage. We experienced such errors very often in practice*" | https://www.ndss-symposium.org/wp-content/uploads/ndss2021_6A-3_24334_paper.pdf |
| in-memory looping → spurious + missed crashes | 论文 (USENIX'23) | "*…it does not reset process state between test cases—overwhelming fuzzers with false-positive crashes caused by polluted heap and global state… leads to both spurious and missed crashes*" | https://www.usenix.org/conference/usenixsecurity23/presentation/stone |
| 计算公式（AFL 2.43b） | 本地源码 | `stab_ratio = 100 - ((double)var_byte_count) * 100 / t_bytes;` `afl-fuzz.c:4525`；红色 `<85%` `afl-fuzz.c:4847` | 仓库内 `afl-fuzz.c` |
| 计算公式（AFL++，相同） | 源码 | `src/afl-fuzz-stats.c:937` 同式；`src/afl-fuzz-ui.c:1466-1470` 同阈值 | https://github.com/AFLplusplus/AFLplusplus/blob/stable/src/afl-fuzz-stats.c |
| `var_bytes` 语义 | 源码 | `if (!var_bytes[i] && first_trace[i] != trace_bits[i]) { var_bytes[i] = 1; …}` `afl-fuzz.c:3096-3097` | 仓库内 `afl-fuzz.c` |
| variable 行为四大成因（未初始化内存/多线程/持久状态/设计性随机） | 官方文档 | 见 `afl_docs/status_screen.txt:297-315` | 仓库内 `afl_docs/` |
| persistent 下 99.xx% 的现实水位 | 社区实测 | "*My code has perfect stability with `__AFL_INIT` … but with `__AFL_LOOP(10000)` the stability drops to 99.xx%*" | https://github.com/AFLplusplus/AFLplusplus/discussions/2082 |
| 多线程可继续 fuzz 的共识 | 社区 | "*Despite a stability decrease, is it still feasible to fuzz multithreaded targets?*"（线程回复肯定可行） | https://groups.google.com/g/afl-users/c/lzDYaClQIco |
| persistent 全局状态 → crash 无法复现的实例 | 社区 | Suricata 案例："*I'm seeing crashes that I can't reproduce… My hypothesis is that it's because I'm keeping some global state*" | https://groups.google.com/g/afl-users/c/xPCQ75vhauI |

---

## 2. 问题 2：与进程所有权解耦的覆盖率方案

### 2.0 关键发现（先读，它改变整个决策）

**fork + 覆盖率在 Windows 上已被发表并开源实现过**——WINNIE (NDSS'21) 及其上的 WinFuzz (USENIX'23)：

> WINNIE 论文 §VI-B："*To address this, we discard dynamic binary instrumentation in favor of fullspeed fuzzing… we patch all basic blocks of the tested program with an int 3 instruction. Then, we fuzz the patched program and wait for the execution to reach a new block. When reached, the first byte of the new block is then restored… **Breakpoints need only be installed once thanks to the fork-server: child processes inherit the same set of breakpoints as the parent.** We noticed that this is an important optimization as … Windows applications easily contain a massive number of basic blocks (e.g., > 100K).*"

> WinFuzz-Winnie 仓库 README（**官方推荐模式**）："*Fork mode with fullspeed instrumentation (recommended): `afl-fuzz -i in -o out -t 1000 -I 1000 -- -bbfile basicblocks.bb -- -harness harness.dll …`"；"*Winnie uses Win32 exception handling to instrument the target program **from within its own address space**. These exception handlers are triggered on events such as breakpoints (int 3), access violations… This is how we are notified about new coverage or crashes.*"

**对 A1 推论的修正**：

| 推论 | 判定 |
|---|---|
| "fork 与 **TinyInst/DR 的懒翻译 + 外部调试器/后台线程**互斥" | ✅ 成立（A1 实验证据充分，不推翻） |
| "fuzzing 必然触发未翻译路径 → fork 拿不到覆盖率" | ❌ **不成立**。fullspeed 不翻译、不 JIT：int3 在 fork 前一次性打满所有 BB；命中后由**进程内异常处理器**（随 CoW 快照被克隆）现场恢复首字节并记录"新 BB"。没有任何"外部线程消费翻译请求"的环节。 |
| "覆盖率与干净状态在当前 WinAFL 架构下互斥" | ❌ 只对 **DR/TinyInst 插桩**成立；换成 fullspeed / 静态改写 / PT 即不互斥（见下表）。 |

### 2.1 候选逐项分析

#### 2.1.1 WinAFL `-Y` 静态插桩（syzygy）

| 项 | 内容 |
|---|---|
| 原理 | 编译期把 AFL 插桩织入 PE（Google syzygy decomposition + afl_transform），`afl-staticinstr.c/h` 提供 `__afl_persistent_loop` 与 `afl_shm_<id>` 共享段（`afl-staticinstr.c:327` `_stprintf(ShmName, … "afl_shm_%s")` —— **与我们 fork-server 的 D1 设计同一共享段，P2.2 已验证 CoW 穿透**） |
| x64 支持 | ❌ **硬性 32 位**：`afl-staticinstr.h:31-33`：`#error Static instrumentation is only available for 32 bit binaries`；`readme_syzygy.md`：syzygy 只 "*decompose PE32 binaries **with full PDB**" |
| 闭源 DLL 适用性 | ❌ 双重不适用：无 PDB（syzygy 要求 full PDB）+ x64。**否决** |
| 与 clone 共存 | 架构上完全兼容（静态代码 + `afl_shm_<id>`），但对我们目标不可用 |
| 链接 | 本地 `readme_syzygy.md`、`afl-staticinstr.h`；https://github.com/google/syzygy |

**x64 替代已存在（重要修正）**：

- **peafl64（Sentinel-One）**：原文 README："*peafl64 is a static instrumentation tool for x64 PEs in Windows… instrumentation adds code to the start of every basic block… **Compatible with WinAFL** (headers included) and kAFL"*；"*Handles relocations, exception tables, relative instructions, jump tables, imports, exports and more*"（含 PE checksum 修复）；基于 wmliang/pe-afl（BlueHat IL 2019）加 x64 支持；需要 **IDA 7+ 分析产出 dump.json**。
  - 成熟度：⚠️ TODO 里有 "*Add tests*"、"*New exception handlers (cxx4)*"——**无系统测试**；由 SentinelLabs 内部使用（其 kAFL 集成说明是实装过的）。
  - 链接：https://github.com/Sentinel-One/peafl64 、https://github.com/wmliang/pe-afl
- **PeAR（GTIRB）**：README 原文："*PeAR is a binary instrumentation tool built using the GTIRB framework that can add AFL++ or **WinAFL instrumentation to x64 Linux and x86/x64 Windows binaries**"*；提供 `--gen-binary WinAFL --target-func 0x401000`，带 Windows 测试管线（`pytest … --winafl32-afl-fuzz-loc … --winafl64-afl-fuzz-loc`）；有配套论文（arXiv 2606.02126，4.25 CPU-yr 评估）。
  - 依赖：GTIRB（有 Windows release 包）、Python；链接：https://github.com/pear-labs/PeAR 、https://arxiv.org/pdf/2606.02126

**结论：`-Y`(syzygy) 对本目标否决；peafl64 / PeAR 是 x64 闭源 PE 静态插桩的现实选项（细节见 §2.1.4 横向对比与取舍矩阵）。**

#### 2.1.2 Intel PT（`-P`）

| 项 | 内容 |
|---|---|
| 原理 | CPU 硬件分支追踪，ToPA/环形缓冲由内核驱动（ipt.sys/WinIPT）管理，fuzzer 侧 `StartProcessIptTracing(child_handle, …)` 按**进程句柄**启停、`GetIptTrace(child_handle)` 取回再解码（`winaflpt.c:1449-1475`；**非 persistent_trace 时每迭代 start/stop 一次**，即"每执行一次、按句柄重新配置"的路径已经存在） |
| 与 clone 共存 | **架构上可行**（缓冲在内核、按句柄配置，克隆体是新进程/新 CR3 → 只需把 clone 的句柄纳入启停流程，属 P3 协议扩展）；但当前实现的**迭代控制和定位 target method 靠 fuzzer 侧调试器 + 软件断点**（`readme_pt.md:9`："*WinAFL opens the target in a debugger*"；`winaflpt.c` 的 `winafl_breakpoint` 链表）——fork 模式下这些 int3 会在克隆体内失去消费者，**必须重构掉** |
| 实际性能 | 硬件低开销（~15% 记录开销），但**解码在 fuzzer 侧**；readme_pt.md:51：环形缓冲回卷会**丢覆盖**（→ 不稳定边）；WINNIE 评语（WinFuzz-Winnie README）："*The truth is that it sucks so don't use it… (1) WinIPT driver is finnicky (2) **Trace buffer overflows easily, leading to poor stability** (3) The trace packets are an unstoppable deluge… slow.*" |
| 成熟度 | 官方 WinAFL 支持，但 WINNIE 实测：59 目标中 WinAFL-PT **33 个跑挂**（"*WinAFL-IPT failed to run 33 of out 59 harnesses*"）；已知故障案例 https://github.com/googleprojectzero/winafl/issues/244 ；VBS/Hyper-V 环境直接失效 https://github.com/intelpt/WindowsIntelPT/issues/19 |
| 本机可用性 | ❌ **A0 实测 `err=87`（ERROR_INVALID_PARAMETER）**（`README-FORK.md:108-112`，Alder Lake 混合核）——在本机当前就是死路；换非混合核 + 关 VBS 的 Intel 机器才可再试（根因未定位，见 §8 #4） |
| 闭源 DLL 适用性 | 好（免改写、免符号；`-coverage_module libvips-42.dll` 过滤即可） |
| 结论 | **不作为主线**。理论耦合度最低、现实最脆（驱动/缓冲/VBS/混合核四重风险 + Winnie 团队的明确劝退）。仅当换到可用硬件且 B2/B3 都不满足时作为备选 |

#### 2.1.3 静态二进制改写工具（Zafl / RetroWrite / Dyninst / E9AFL / 其他）

| 工具 | 平台支持（证据） | 判定 |
|---|---|---|
| **RetroWrite** | AFL++ 官方文档：*"...if you have an x86_64 or arm64 binary that … still has its symbols and compiled with position independent code (PIC/PIE)"* —— **ELF 前提，无 PE 支持** | ❌ 否决（非 Windows PE） |
| **E9AFL / e9patch** | e9patch 面向 ELF x86-64（https://github.com/GJDuck/e9patch ）；AFL++ 文档未列 PE | ❌ 否决 |
| **ZAFL** | AFL++ 官方首推（*"ZAFL is a good choice"*；*x86-64 C/C++, stripped/unstripped, PIE/non-PIE*），但发布/文档面向 Linux 工具链，**未见 Windows PE 支持声明**（需确认，见 §8 #5） | ❌ 暂否决 |
| **Dyninst / afl-dyninst** | Dyninst 本身有 Windows 平台支持，但 AFL++ 官方对静态插入的评价：*"it is a non-trivial problem to insert instructions, which change addresses… **more often than not binaries crash** when they are run"*；afl-dyninst 项目以 Linux 为主 | ⚠️ 原理上"改写后保存再跑"= 静态产物、与 fork 兼容，但**正确性风险高**，不推荐首选 |
| **peafl64 / PeAR** | 见 §2.1.1：**明确支持 Windows x86/x64 PE + WinAFL 插桩** | ✅ 仅这两个进入候选 |
| Mcsema | 抽取到 LLVM IR；对 20MB 闭源 C++ DLL 不现实 | ❌ 否决 |

**与 clone 共存性（对所有静态改写通用）**：✅——插桩代码织入磁盘二进制、覆盖率写 `afl_shm_<id>`（peafl64/PeAR 均对接 WinAFL 头），fork 子进程通过 CoW 继承代码页与 SHM 映射（**P2.2 已实测 SHM 穿透**）。
**闭源适用性（通用风险）**：改写的是磁盘副本 → "插桩后的目标 ≠ 真实目标"（反汇编误判、间接跳转/jump table 漏改、C++ 异常表 xx4 等）；DLL 签名会失效（本地 harness `LoadLibrary` 场景无妨，fuzzing 不校验签名）。

#### 2.1.4 驱动/内核级快照

- **Windows 本机内核快照**：WinFuzz 摘要直接给结论："*The closed-source nature of its **Windows kernel** prevents current kernel-based snapshotting techniques from being ported*" → Linux 的 AFL-Snapshot-LKM（https://github.com/AFLplusplus/AFL-Snapshot-LKM ）无法移植，**也不存在等价开源实现**（该摘要即为权威依据）。
- **虚拟机/路线**：kAFL 支持 Windows guest（官方教程 https://intellabs.github.io/kAFL/tutorials/windows/index.html ，QEMU/KVM + Windows VM 模板）；peafl64 还提供"静态插桩 + helper driver + kAFL hypercall"的组合。**成本评估**：需 Linux 宿主 + VM 模板 + guest 内 agent/helper driver，构建-部署链以周计；对一个**用户态 DLL** 来说，快照只解决"速度"，不解决"我们缺的东西"——而我们已有 233 clones/sec 的用户态 fork。**否决：成本/收益与既有 P2 完全不成比例。**

#### 2.1.5 目标内快照（WinFuzz，USENIX'23）

| 项 | 内容 |
|---|---|
| 原理 | 摘要原文："*combines **binary- and library-level hooking** to allow applications to snapshot themselves—while leaving both their source code and the Windows kernel untouched*"——Detours hook 堆分配器 + guard page 追踪全局区 + 寄存器状态恢复汇编（论文 Listing 2），每迭代恢复到快照 |
| 性能 | "**7–182x increased performance**"（vs 进程创建 7.0x/182.65x、vs Winnie fork 7.00x 平均——Table 3） |
| 32 位限制 | ✅ 用户所述属实：仓库 README 原文："***Currently, only 32-bit builds are supported (inline asm).***"（inline asm：寄存器恢复、`mov [stack_pointer], ebp` 堆钩子等，均为 x86 语法） |
| x64 移植工作量 | 中等偏上，非小改：inline asm → x64 MASM；**x64 VEH/异常处理需要正确 unwind 信息**；堆钩子要覆盖目标实际使用的分配器（libvips-42.dll 经 libglib → CRT/Rtl heap，需逐层确认钩子覆盖面）。**估计 1–2 周 + 适配风险**（见 §8 #6） |
| 与 clone 关系 | **替代关系而非配合关系**（自快照取代 fork）。注意 WinFuzz 是"forkserver + target-embedded"并存的架构（WinFuzz-Winnie repo 同时含 forklib 与 injected-harness-winfuzz） |
| 闭源 DLL 适用性 | 需要能对目标进程注入 hooked harness（Winnie 的 injected-harness 注入方式）——对我们的 harness+DLL 组合可行 |
| 覆盖率来源 | WinFuzz 用的也是 **coverage-guided tracing（fullspeed 布尔 BB）**，不是 DR edge——即它同样不提供"DR 式 hitcount 覆盖"（论文 §6.4 专门讨论了两者差异） |
| 开源 | https://github.com/FoRTE-Research/winfuzz （主仓库）；https://github.com/FoRTE-Research/winfuzz （WinFuzz atop Winnie，含 forklib + winfuzz hook 实现） |
| 判定 | **可行备选**（若我们不想做 fork）；优先级低于"E1–E6 先验证 B2"和"fork+fullspeed"，因 x64 移植与分配器适配都是净新增工作 |

#### 2.1.6 硬件断点 / 单步 + 自建边表

- **硬件断点**：x86 调试寄存器只有 **4 个（Dr0–Dr3）且是 per-thread 状态**（MSDN `CONTEXT` 结构，https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context ）——对一个 20MB DLL 的数万 BB，覆盖率粒度完全不够；且 fork 只克隆调用线程，DR 寄存器随线程上下文走，其他线程无从生效。
- **单步（TF 标志）**：每条指令触发一次 `EXCEPTION_SINGLE_STEP`（内核往返 + 上下文保存），按每条异常 1–5µs 估算 vs 指令 2–5ns → **500–2000x 级开销**，fuzzing 不可用（工程判断，见 §8 #7 标注为分析性结论）。
- **判定：❌ 否决**。这一思想的**可行版本**正是 WINNIE 的 fullspeed 软件 int3（一次性打点、新 BB 才异常、近原生速度）——学界已经替我们做了这个取舍。

#### 2.1.7 其他顺带排除项

- **Frida/GumStalk**：进程内 JIT 运行时 + 自有工作线程，与 DR/TinyInst 同类（跨线程运行时状态 + 需持续管理目标进程）；AFL++ Frida mode 面向 Linux/macOS，WinAFL 无此模式。❌
- **Wine+QEMU 模式**：AFL++ 文档 "*Wine mode can run Win32 PE binaries*"——**仅 32 位**且需 Linux+Wine 宿主，目标是 x64 QQ DLL。❌
- **unicorn_mode**：AFL++ 文档对 non-linux 的兜底是 "*use unicorn_mode*"——需要为 x64 Windows 目标手写全系统仿真 + 外设/API 模型（glib/vips 全家桶），工作量以月计。❌

### 2.2 AFL++ 官方文档对 Windows 目标的推荐（原文核对）

`fuzzing_binary-only_targets` TL;DR 原文：

> "*FRIDA mode and QEMU mode in persistent mode are the fastest - if persistent mode is possible and the stability is high enough. Otherwise, try Zafl, RetroWrite, Dyninst… If your target is non-linux, then **use unicorn_mode**.*"

- 静态重写名单（Zafl/RetroWrite/Dyninst）实质是 **ELF/Linux 生态**（见 §2.1.3 逐项核对）；
- DynamoRIO 条目：WinAFL "*very good but windows only*"，且同文档称 DR "*speed decrease of 98-99%*"——**正是我们的排除项**；
- **结论：AFL++ 官方对"Windows + 闭源 + x64 + 用户态 DLL"没有更好的推荐方案**；这个生态位的实际答案就是 WinAFL 系（DR/PT/TinyInst/静态改写）+ WINNIE/WinFuzz 路线，而官方文档的 "stability high enough" 前提恰好呼应了我们的问题——**在 Windows 上找到"high stability + 高吞吐"的组合，本身就是 WINNIE/WinFuzz 两篇论文的贡献点**。

### 2.3 "插桩一次、快照复用"是否存在？——存在，三条已验证路径

| 路径 | 机制 | "快照复用"如何成立 | 证据状态 |
|---|---|---|---|
| **① Winnie 式 fullspeed int3（首选）** | 启动时按 `.bb` 文件（IDA `bb.py` / **Ghidra `bb_ghidra.py`**，闭源无符号可用）把每个 BB 首字节改为 int3；进程内 VEH/UEF 处理：新 BB → 记录覆盖 + 恢复首字节；崩溃 → 走 pipe 上报 | int3 字节、VEH 表、SHM 映射、覆盖率状态**全部在进程内存里**，`RtlCloneUserProcess` CoW 全量继承；**打点一次，子子孙孙继承**（论文原句见 §2.0） | ✅ 论文 + 开源 + 官方 README 标注 "recommended"；**本仓库 fork(P2) 未与之集成，属 P3 新工作** |
| **② 静态改写（peafl64 / PeAR）** | 插桩代码编译进目标二进制，写 `afl_shm_<id>` | 磁盘产物本身就是插桩后的；fork 继承即可 | ✅ 工具开源；与 fork 兼容靠架构保证 + P2.2 SHM 实测；⚠️ 工具成熟度中低 |
| **③ Intel PT** | 硬件追踪、内核缓冲 | 追踪状态不在目标进程内；每 clone 按句柄重新 Start/Stop（现有 `persistent_trace=off` 路径即此模式） | ⚠️ 架构可行、本机硬件/驱动不可用 + 官方劝退 |

---

## 3. 问题 3：libvips 的 16.72% 究竟由什么造成？

### 3.1 已掌握的证据（含本次新增的本地审阅）

**新发现 1——我们的 harness 缺少全部三项确定性设置（`E:\fuzzwork-real\harnesses\qq_image_vips\src\harness_vips.c` 全文审阅）：**

```c
// 现状（节选）：
p_new_from_buffer(buf, n, "", NULL);   // 返回值直接丢弃：不 unref → 每次迭代泄漏 VipsImage/VipsSource
// main() 只调 vips_init：
if (p_init) p_init("qq_image_vips");
// 未调用：vips_concurrency_set / vips_cache_set_max / vips_shutdown / vips_error_clear / g_object_unref
```

→ **泄漏（H-A 加重项）+ 缓存状态（H-A）+ 默认多线程（H-B）三个问题同时在场。**

**新发现 2——OSS-Fuzz 官方 libvips harness 恰好修掉了这三个问题**（`libvips/fuzz/vips_fuzzer.cc`，`LLVMFuzzerInitialize` 原文）：

```c
if (VIPS_INIT(*argv[0])) return -1;
vips_concurrency_set(1);     // ← 单线程
vips_cache_set_max(0);       // ← 关闭 operation cache
// + vips_operation_block_set() 黑名单（防超时/防写 stdout/防 tmp 残留）
```

**新发现 3——libvips 状态源的源码级证据：**

| 状态源 | 证据 |
|---|---|
| 线程池默认=CPU 核数 | 官方文档："*by default it will use as many threads as you have CPU cores. Use `vips_concurrency_set()` to change this*"（https://www.libvips.org/API/current/using-threads.html ） |
| `VIPS_CONCURRENCY` 环境变量 | 官方 API 文档：default 时 "*the number of threads is set by the environment variable `VIPS_CONCURRENCY`, or if that is not set, the number of threads available on the host machine*"（https://www.libvips.org/API/current/func.concurrency_set.html ）→ **E1 无需改代码** |
| **operation cache（默认 100 条，跨迭代状态）** | `libvips/iofuncs/cache.c`：`static int vips_cache_max = 100;` + LRU 淘汰；文档："*Instead of calling vips_object_build(), call vips_cache_operation_build()… hash from the operations' input arguments*"——**同一输入第二次调用可能直接命中缓存、跳过整个解码路径 → 校准时第 1 次与第 N 次覆盖不同 → var_bytes 爆炸** |
| foreign load 默认**允许**进缓存 | `libvips/foreign/foreign.c`：仅当"*seq loaders running in seq mode*"才 `nocache=TRUE`；random-access（buffer 加载默认）→ **走缓存** |
| fork 兼容性隐患（源码注释） | `threadpool.c`："`max_threads > 0` will create a set of threads on startup… **but may break on systems that try to fork() after init**" → 即使走 fork 路线，也必须 `VIPS_CONCURRENCY=1` |
| 网络 API 导入（H-C） | 本地 `dumpbin /imports libvips-42.dll`：**WS2_32**（WSAStartup/WSARecv/WSASend/WSAWaitForMultipleEvents/accept…）、**DNSAPI**（DnsQuery_A/DnsFree）、**IPHLPAPI**（GetAdaptersAddresses/GetIpForwardTable2…）——典型的 libcurl 静态链接特征（curl 初始化会枚举网卡）。**但"导入≠每次迭代调用"**：`vips_image_new_from_buffer` 不走 HTTP 源，网络路径大概率只在 init 阶段或根本不触发 → H-C 判为**低嫌疑，待 E5 证伪** |
| OSS-Fuzz 持续运行 libvips | 项目页 20 个 fuzzers 持续 fuzz（https://github.com/google/oss-fuzz/tree/master/projects/libvips ；introspector profile "Fuzzer count: 20"）——**未见公开的 stability 讨论**（该指标 AFL 专属，libFuzzer 无此概念）→ §8 #2 |

**已知口径提示**：玩具 `test.exe` 98–100% vs libvips 16.72%（引擎各自在其玩具上都接近满分）→ **引擎无辜，目标/harness 有罪**，这一判断与本节证据完全吻合。

### 3.2 对假设 A/B/C 的判定

| 假设 | 判定 | 理由 |
|---|---|---|
| **A：persistent 状态污染（WINNIE 论点）** | **主嫌（H-A，多个具体机制）** | ① operation cache 默认开 100 条（源码证据）→ 跨迭代命中改变路径；② harness 每次调用泄漏 VipsImage（源码证据）→ 堆单调增长；③ `vips_error_buffer` 不清理（全局累积状态，AFL 文档四大成因之一）；④ persistent 模式本身 = 官方点名的发散机制 |
| **B：libvips 自身多线程（GLib 线程池）** | **次嫌（可一步隔离）** | 默认并发=核数（文档证据）；多线程 = AFL 文档四大成因之一；**`VIPS_CONCURRENCY=1` 一条环境变量即可定量验证（E1）** |
| **C：网络/系统依赖** | **低嫌疑** | 静态导入真实存在（dumpbin 实测），但 buffer 加载路径不构造 HTTP 源；网络 API 属"外部数据/时序"类不确定源（AFL 文档成因之一），E5 低成本证伪 |

**注意：A 与 B/C 正交**——E1 验 B，E2 验 A，E3 拆 A vs "进程外因素"，E4 定位不稳定在哪个模块。

### 3.3 实验设计（可执行、成本可控、不与既有基线重复）

> 全部实验复用 §4.2 材料；每轮 10 分钟（与 BASELINE 同口径），读 `fuzzer_stats` 的 `stability / bitmap_cvg / execs_per_sec`。完整命令见 §6。

| 实验 | 操作 | 假设 | 判读 |
|---|---|---|---|
| **E1** | 只加 `set VIPS_CONCURRENCY=1`，其余照基线 | B | stability 显著↑（如 ≥60%）→ B 是主因之一；无变化 → B 排除 |
| **E2** | 改 harness：`vips_concurrency_set(1)` + `vips_cache_set_max(0)` + 每次 `g_object_unref` 返回图 + `vips_error_clear()` | A+B | stability ≥90% → **B2 成立，收工**；仍 <80% → A 的剩余机制（或进程外因素） |
| **E3** | 去掉 `-iterations 2000 -persist -loop`（非 persistent，每例新进程），同种子同超时 | 拆 A vs B/C | 非 persistent 显著高于 persistent → 污染来自进程内累积（A）→ "不持久化也能修"（官方建议路径）或 fork；两者都 ~17% → 不确定性在"每次执行内部"（线程时序/环境）→ B/C/E5 |
| **E4** | 只 `-instrument_module harness_vips.exe`（不插桩 libvips） | 定位 | harness-only 稳定（预期 ~100%，同 `test.exe`）→ 不稳定源在 DLL/库层；harness-only 也低 → harness 自身（则先修 harness） |
| **E5** | 断网（关 Wi-Fi/拔线）重跑 E1 配置；可选 Procmon/API Monitor 观察 `DnsQuery/GetAdaptersAddresses` 是否被调用 | C | stability 差值 ≈0 → C 排除；差值明显 → C 成立（隔离网络） |
| **E6** | 线程观测：`fuzz_one` 内打印 `GetCurrentThreadId()`（临时补丁）统计每进程线程数；或 E1 已隐含 | B 辅证 | 并发=1 时线程数恒定 → B 被控住 |

**判读矩阵 → 路线决策**：

```
E2 后 stability ≥ 90% 且 bitmap_cvg 诚实口径不降（~~≥12.7%~~ 已修正：见 attribution，≈2.6%） 且吞吐 ≥ 0.5×43.83
    └─→ B2 达标：不需要 fork、不需要换引擎（P5 只需把两行配置写进 fuzzkit）
E2 后 80–90%：达标边缘 → 追查残留（E5/E6 + var_bytes 索引对比）
E2 后仍 < 80%，但 E3（非 persistent）≥ 90%
    └─→ 进程内累积是根因，官方建议"非 persistent"；若吞吐不可接受 → B3（fork）
E2 后 < 80%，E3 也 < 80%
    └─→ 执行内部的时序不确定性（线程未控住/系统调用）→ 继续隔离；
        同时 B3（fork + fullspeed）成为唯一能同时保证"干净态+覆盖率"的架构方案
```

### 3.4 如果根因是 B/C，是否不需要 fork？

**是——明确的"不需要"。** 条件与理由：

- B（线程）：`VIPS_CONCURRENCY=1` 后执行变成**单线程顺序执行**，persistent 循环内只要无状态残留即确定性执行；C（网络）：隔离后同理。此时 persistent 模式满足 AFL++ 的前提（"target 是 pure function"），官方也不再建议退出 persistent（best_practices 的警告条件是"persistent 不稳定而非 persistent 稳定"——两者都稳定就没问题）。
- 支撑：玩具目标同一引擎（TinyInst）已实测 **100.00%**——引擎在"确定性目标"上能给出满分，**修目标即可，不用换引擎（B1）也不用 fork（B3）**。
- ⚠️ 但若 B2 之后**仍需"crash 100% 可重放"的硬保证**（BASELINE 判据之一），单靠调参是概率性保证（§8 #1：无定量重放研究）；fork 提供的是**结构性保证**（每例干净态）。这是 B3 仍然保留为"终局方案"的原因。

---

## 4. 取舍矩阵

| 路线 | stability 预期 | 吞吐预期 | 改造成本 | 闭源 x64 DLL 适用 | 成熟度 | 主要风险 |
|---|---|---|---|---|---|---|
| **B2：调参/隔离（TinyInst persistent + 单线程 + 关缓存 + 修 harness）** | **80–100%（若根因命中）** | ≈43.83/s（不变） | **0.5–1 天** | ✅ | 高（引擎现成） | 可能仍 <80%（根因不止 A/B）；crash 重放是概率性而非结构性保证 |
| B1：DR persistent（真实 libvips） | **预期 ≈17%（大概率失败，作对照）** | ≈40–50/s | 1 小时 | ✅ | 高 | 浪费 1 小时换一个"引擎无关"的确认（值得） |
| 非 persistent（每例新进程） | 高（若 A 主导）；= 低（若 B/C 主导） | **低**（每例 LoadLibrary 20MB DLL + glib init；Winnie 论文：CreateProcess <100/s，本目标估计 5–15/s） | 0（改命令） | ✅ | 高 | 吞吐可能跌破 43.83×0.5 判据；官方建议但代价大 |
| **B3-a：fork + fullspeed 进程内 int3（Winnie 式）** | **≥95%（结构性：干净态 + 单线程 + 固定打点）** | clone 上限 233/s，int3 近原生 → 预估 **≥50–100/s** ✅ | **3–5 天**（.bb 生成 + int3 打点器 + VEH 处理器 + 接入 P2/P3 协议） | ✅（Ghidra 脚本 `bb_ghidra.py` 无符号可用） | 中（论文+开源参考，未在本仓库集成） | 覆盖率是**布尔 BB**（弱于 edge+hitcount，队列信号较粗）；多线程目标需先控线程 |
| **B3-b：fork + 静态改写（peafl64/PeAR）** | ≥95%（同上 + 全量 edge 覆盖） | clone 上限 233/s，织入开销 10–30% → **≥100/s** ✅ | **1–2 周**（IDA 分析 20MB DLL + 改写验证 + x64 静态模式 fuzzer 侧对接 + P3） | ✅ 但"改写产物≠真实目标" | **中低**（peafl64 无测试 TODO；PeAR 较新） | 反汇编/异常表(xx4)/jump table 漏改 → 目标行为漂移或直接崩；20MB DLL 分析耗时 |
| fork + Intel PT | 受缓冲回卷影响（Winnie: poor stability） | 硬件低开销但解码贵 | 3–7 天 + **换非混合核 Intel 机 + 关 VBS** | ✅（免改写） | 中低（官方支持但故障率高：33/59 跑挂） | **本机 err=87 不可用**；驱动脆；必须重构调试器式迭代控制 |
| WinFuzz x64 移植（目标内快照） | ≥95%（论文核心论点） | 论文 7–182×（vs re-exec/Winnie）→ 远超判据 | **1–2 周**（inline asm→x64 + unwind + 分配器钩子适配） | ⚠️ 32 位 only（README 原文），x64 需移植 | 中（论文+开源） | 分配器钩子是否覆盖 libglib/CRT 未验证；与我们 fork 引擎是替代关系（前期 P2 投资闲置） |
| 内核/虚拟机快照（kAFL-Windows 等） | ≥95% | 受 VM 折损 | **数周**（Linux 宿主+VM 模板+guest agent） | ⚠️ 需 guest 内 agent/helper driver | 中（kAFL 官方教程） | 成本与收益完全失衡；WinFuzz 摘要已证 Windows 本机内核快照不可移植 |
| 硬件断点/单步 | — | — | — | — | — | **否决**：4 个 DR 寄存器 per-thread 粒度不足；单步 ~500–2000x 开销 |
| （已否决静态工具）syzygy `-Y` / RetroWrite / E9AFL / ZAFL / unicorn / Wine | — | — | — | ❌ 32 位+PDB / ELF-only / 仿真 | — | 见 §2.1.3 |

---

## 5. 推荐路线与否决理由

### 5.1 推荐路线（两阶段 + 一个架构性结论）

**第 0 步（立即，1–2 天）：跑 E0–E6，验证 B2。**
理由：证据最直接（OSS-Fuzz 官方 libvips harness 的三件套设置我们一个都没做）、成本最低（改 harness 两行 + 环境变量）、且它是后续所有分支的公共前置（**无论走不走 fork，`VIPS_CONCURRENCY=1` + 关缓存 + 修泄漏都必须做**——fork 路线也需要单线程来规避"只克隆调用线程"的死锁问题，Winnie 论文原话："*only the thread calling fork is cloned… To sidestep this problem, we target deeply-nested functions that behave in a thread-safe fashion*"）。

**第 1 步（1 小时，旁证）：DR persistent 真实目标对照（B1）。**
预期 ≈17%（与 TinyInst 一致），价值在于**钉死"引擎无关"**，防止后续走弯路；若意外 ≥95%，则直接达标（低概率，但成本只有 1 小时）。

**第 2 步（若 B2 未达标）：继续 P3，但覆盖率来源改为"插桩一次、快照复用"**：
- **首选 B3-a（fork + in-process int3 fullspeed）**：与我们已完成的 P2 fork 引擎（RtlCloneUserProcess，233 clones/sec，本机 19045 实测）无缝对接；覆盖率状态全在进程内、CoW 天然继承；有 WINNIE 论文 + WinFuzz-Winnie 开源仓库作参考实现（含 `bb.py`/`bb_ghidra.py`、异常处理、fork 协议对接）；Winnie 官方推荐模式就是 "fork + fullspeed"。
- **备选 B3-b（peafl64 / PeAR 静态改写）**：若必须保留 AFL 式 edge+hitcount 覆盖（更细的队列信号），接受 1–2 周成本与工具成熟度风险。
- **明确不再尝试**：DR/TinyInst + clone（A1 已证死路）、`-Y` 静态（32 位+PDB）、本机 PT（err=87）。

### 5.2 不推荐路线及否决理由（逐条）

| 路线 | 否决理由（证据） |
|---|---|
| **B1 作为终局方案**（换 DR persistent 即达标） | ① toy 98.31% 不能外推到真实目标（A1 教训 + 本仓库 16.72% vs 100% 的反例）；② 根因在目标的证据链（§3.2）使 DR 预期同样 ~17%；③ **即便达到 98%，persistent + 闭源仍是官方点名的 crash 不可重放场景（AFL++ FAQ）+ WINNIE/WinFuzz 明确劝退**（README："*persistent mode is unsuitable for fuzzing closed-source applications, which are predominant on Windows*"）——与 BASELINE"crash 100% 可复现"判据冲突 |
| **B4**（16.72% 够用，目标改判） | ① 低于官方唯一给出的"仍可能成功"数字（>20%）；② 处于官方"below 90/80 应采取对策"区间；③ 命中"persistent + 不稳定"最严厉警告（crash 不可重放），与用户核心关注直接冲突；④ 无任何 <20% 案例背书（§8 #1）。**但**：接受其合理内核——目标值从"字面 100%"修正为"≥90%（黄线 95%）+ crash 100% 可重放 + bitmap_cvg ≥12.7%" |
| 本机 Intel PT 主线 | A0 `err=87`（`README-FORK.md:108`）+ VBS/混合核风险 + Winnie 团队明确劝退（driver finicky / buffer overflow → poor stability / 解码慢）+ 33/59 实测跑挂 + 迭代控制依赖 fuzzer 调试器（与 fork 冲突需重构） |
| `syzygy -Y` | `afl-staticinstr.h:31 #error`（x64 禁用）+ `readme_syzygy.md`（PE32 + full PDB）——闭源 x64 双杀 |
| RetroWrite / E9AFL / ZAFL / unicorn / Wine | ELF/32 位/仿真生态，逐条见 §2.1.3、§2.1.7 |
| 内核快照 / kAFL | WinFuzz 摘要：Windows 内核闭源、快照技术不可移植；kAFL 需 VM+agent，周级成本，对用户态 DLL 收益不成比例 |
| 硬件断点/单步 | 4×DR per-thread 粒度不足；单步开销 500–2000x；已被 fullspeed int3 取代 |
| 混合路线 C（persistent + 周期 fork 重置） | 仍需先解决"fork 时插桩引擎不在"的问题（A1），且复杂度高于直接 fork；**若 B2 成功则无必要，若 B2 失败则直接全 fork 更干净** |

### 5.3 对判据 B1–B5 的最终回答

| 选项 | 判定 |
|---|---|
| B1 换引擎 | ❌ 不是正解（预期失败；即使成功也有 persistent 重放问题）——但保留为 1 小时对照实验 |
| B2 调参/隔离 | ✅ **第一优先验证项**（权威先例、成本最低）；**若 E2 达标则它就是答案** |
| B3 覆盖率解耦 + fork | ✅ **架构上的正解（若 B2 被证伪，或需要结构性的 crash 可复现实证）**——P3 应继续，覆盖率来源用 fullspeed int3（首选）/peafl64（备选），不是 DR/TinyInst |
| B4 16.72% 够用 | ❌ 否决；但**目标值本身要修正**：≈100% → **≥90%（工程 95%）+ crash 100% 可重放 + bitmap_cvg 不降** |
| **B5 其他** | ✅ **本次交付的正式答案：B5 = "B2 先行（1–2 天）→ 按 §3.3 判读矩阵决定收工或转 B3"的分阶段策略；其中 B3 是唯一的架构性长期正解，B4 的目标修正建议被采纳（100%→90/95%），B1 仅作对照。** |

---

## 6. 可执行的验证步骤（命令级）

### E0 基线复核（可选，避免重复劳动可跳过——数据已在 `out_ti` / `BASELINE.md`）

```bat
findstr /C:"stability" /C:"bitmap_cvg" /C:"execs_per_sec" E:\GitCode\winafl\dist\winafl-portable\out_ti\fuzzer_stats
```

### E1 环境变量单线程（假设 B，无需改代码，~10 分钟）

```bat
cd /d E:\GitCode\winafl\dist\winafl-portable
set VIPS_CONCURRENCY=1
afl-fuzz.exe -i in_vips -o out_e1_conc1 -t 20000 -I 15000 -y -- -instrument_module libvips-42.dll -target_module harness_vips.exe -target_method fuzz_one -nargs 1 -iterations 2000 -persist -loop -- E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\harness_vips.exe @@
:: 运行 10 分钟后 Ctrl+C，读数：
findstr /C:"stability" /C:"bitmap_cvg" /C:"execs_per_sec" out_e1_conc1\fuzzer_stats
```

（命令口径 = `E:\fuzzwork-real\baseline\fuzzer_stats_persistent.txt:28` 的原始基线命令，仅新增环境变量。）

### E2 harness 三件套（假设 A+B，改代码后重编，~半天）

修改 `E:\fuzzwork-real\harnesses\qq_image_vips\src\harness_vips.c`：

```c
/* main() 中，vips_init 之后追加： */
typedef void (*PFN_conc_set)(int);
typedef void (*PFN_cache_max)(int);
typedef void (*PFN_error_clear)(void);
typedef void (*PFN_unref)(void *);
PFN_conc_set  p_conc  = (PFN_conc_set)GetProcAddress(h, "vips_concurrency_set");
PFN_cache_max p_cmax  = (PFN_cache_max)GetProcAddress(h, "vips_cache_set_max");
PFN_error_clear p_eclr= (PFN_error_clear)GetProcAddress(h, "vips_error_clear");
HMODULE hg = GetModuleHandleA("libgobject-2.0-0.dll");
PFN_unref p_unref = hg ? (PFN_unref)GetProcAddress(hg, "g_object_unref") : NULL;
if (p_conc) p_conc(1);
if (p_cmax) p_cmax(0);          /* OSS-Fuzz 同款：关闭 operation cache */

/* fuzz_one() 中： */
void *im = p_new_from_buffer(buf, n, "", NULL);
if (im && p_unref) p_unref(im); /* 消除每迭代泄漏 */
if (p_eclr) p_eclr();           /* 清空全局错误缓冲 */
```

构建后用 E1 同一命令跑 `out_e2`（先 `set VIPS_CONCURRENCY=` 清掉，验证代码路径本身）。**判读：`stability ≥ 90%` 且 bitmap_cvg 诚实口径（≈2.6%，旧 12.7% 为 lazy-init 虚高） → B2 达标。**（✅ 实战结果：99.01%）

### E3 非 persistent 对照（拆 A vs 进程外因素，~10 分钟）

```bat
cd /d E:\GitCode\winafl\dist\winafl-portable
afl-fuzz.exe -i in_vips -o out_e3_np -t 20000 -I 15000 -y -- -instrument_module libvips-42.dll -target_module harness_vips.exe -target_method fuzz_one -nargs 1 -- E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\harness_vips.exe @@
:: 即 E1 命令去掉 "-iterations 2000 -persist -loop"
findstr /C:"stability" /C:"bitmap_cvg" /C:"execs_per_sec" out_e3_np\fuzzer_stats
```

> 口径已核对：DR/TinyInst 覆盖率键是**模块相对偏移**（`winafl.c:318` `offset = start_pc - mod_entry->data->start`；`tinyinst_covmap.cpp:134` `GetBBOffset(module, bb_address)`），**ASLR 不污染跨进程 stability**，该对照有效。
> （非 persistent 下 execs/sec 会明显下降——这是 E3 同时产出的吞吐数据，用于评估"官方建议的非 persistent 路线"是否可接受。）

### E4 模块定位（不稳定在 harness 还是 DLL，~10 分钟）

```bat
afl-fuzz.exe -i in_vips -o out_e4_harness -t 20000 -I 15000 -y -- -instrument_module harness_vips.exe -target_module harness_vips.exe -target_method fuzz_one -nargs 1 -iterations 2000 -persist -loop -- E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\harness_vips.exe @@
```

### E5 网络因素（假设 C，~10 分钟）

关网（禁用 Wi-Fi/拔线）后重跑 E1 命令 → `out_e5_nonet`；对比 E1 的 stability 差值。
辅助（可选）：API Monitor / Procmon 观察一轮执行中 `DnsQuery_A`、`GetAdaptersAddresses` 是否被调用。

### E6 线程观测（辅助 B 判读）

临时在 `fuzz_one` 打印 `GetCurrentThreadId()`，分别在 E1 前后统计每进程内出现过的线程 ID 集合；或直接以 E1 差值作为 B 的判据。

### B1 对照（DR persistent 真实目标，1 小时；来自 HANDOVER §5.2）

```bat
cd /d E:\GitCode\winafl\dist\winafl-portable
afl-fuzz.exe -i in_vips -o out_dr_vips -t 20000 -D dynamorio\bin64 -w winafl.dll -- -coverage_module libvips-42.dll -target_module harness_vips.exe -target_method fuzz_one -nargs 1 -- E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\harness_vips.exe @@
findstr /C:"stability" /C:"bitmap_cvg" /C:"execs_per_sec" out_dr_vips\fuzzer_stats
```

### 若转 B3：首个 PoC 步骤（1 天内可验证可行性）

```bat
:: 1) 生成 libvips-42.dll 的 BB 列表（Ghidra，无需符号）：
::    用 WinFuzz-Winnie 仓库自带 bb_ghidra.py -> libvips.bb（格式：[module] + rva,fileoffset）
:: 2) 写最小 PoC（可基于 fork-poc/dr_clone_probe3.c 骨架）：
::    LoadLibrary(libvips-42.dll) -> 按 .bb 打 int3 -> 安装 VEH(记录新BB+恢复字节) ->
::    RtlCloneUserProcess x100 -> 验证：克隆体正常解析图片、无 0x80000003、
::    新 BB 事件在克隆体内被进程内处理器消费、afl_shm 覆盖率单调增长
:: 3) PASS 则把 VEH 记录接入 P2 协议（'K'/'C' 前 flush map），进入 P3 集成
```

### crash 可复现性验证（任一路线通用）

对 fuzzing 产出的每个 `crash:*`：`afl-showmap` / 直接运行 harness 重放 10 次，记录复现率；目标 = 100%（BASELINE 判据）。

---

## 7. 不确定性清单（证据不足，需进一步实验/确认）

| # | 不确定性 | 影响 | 消除方式 |
|---|---|---|---|
| 1 | **"低 stability 仍有效挖 bug"没有任何 <20% 的案例研究；stability→crash 重放成功率无定量研究** | 影响 B4 讨论的上限 | 无法用文献消除；只能靠 E2/E3 后实测 crash 复现率（§6 末） |
| 2 | libvips 在 OSS-Fuzz 的**稳定性/flakiness 公开记录**未找到（libFuzzer 无 stability 指标，AFL 生态无人公开讨论 vips） | 弱化"E2 必然达标"的置信度 | E1/E2 直接实验 |
| 3 | `vips_image_new_from_buffer` 返回的 lazy image **在不 sink 的情况下实际解码到哪一层**（我们的 harness 不评估、不 unref）；可能意味着当前 12.73% 覆盖主要来自 header 解析 | 影响"攻击面是否真的 fuzz 到"（超出 stability 的独立问题） | litecov 观察 `vips_sink`/`vips_image_write` 是否被调；E2 补丁可加显式 sink 对比覆盖变化 |
| 4 | A0 PT `err=87` 的**根因未定位**（混合核 / VBS / ipt.sys per-process tracing 限制三者未分离） | 决定"换非混合核 Intel 机"是否值得 | 在关 VBS + `winipt` 自带 `ipt.exe` 复测；查 CPUID.14H PT 能力位；见 https://github.com/googleprojectzero/winafl/issues/244 |
| 5 | **ZAFL 是否支持 Windows PE** 未从一手文档确认（AFL++ 文档未提平台） | 影响静态改写备选名单 | 读 https://git.zephyr-software.com/opensrc/zafl README 源码 |
| 6 | **WinFuzz x64 移植的真实工作量**与**其堆钩子对 libglib/CRT 分配器的覆盖面**未验证 | 影响"WinFuzz x64"备选的成本估计（现估 1–2 周） | 读 WinFuzz 主仓库 hook 实现 + 在 32 位样本上先跑通 |
| 7 | 硬件断点/单步的开销数字为**工程估算**（无实测/引用） | 不影响结论（否决由 4×DR 粒度决定） | 无需消除 |
| 8 | **fullspeed 布尔覆盖（无 hitcount）对本目标队列质量的实际影响**（WinFuzz 论文 §6.4 显示两者各有胜负：bool 覆盖 edge 更高但漏掉 hitcount 增长型输入） | 影响 B3-a 与 B3-b 的最终取舍 | B3-a 落地后与基线对比 `paths`/`bitmap_cvg` 增速；或直接选 B3-b |
| 9 | Winnie fork（NtCreateUserProcess+CSRSS offsets）**在 Win10 22H2 (19045) 未被 WinFuzz 论文测试过**（他们只测了 19043/19044） | 若参考其 fork 实现需注意 | **我们不必改**：P2 的 RtlCloneUserProcess 方案已在本机 19045 实测 10000 次 0 失败；仅参考其覆盖率侧 |

---

## 8. 总证据表（结论 → 来源 → 原文 → 链接）

| # | 结论/证据要点 | 来源类型 | 原文引用（节选） | 链接 |
|---|---|---|---|---|
| 1 | stability 定义、路径识别困难 | 官方文档 | "*Stability is measured by how many percent of the edges…*" | https://aflplus.plus/docs/faq/ |
| 2 | persistent 不稳定 → crash 不可重放 + 缓解手段 | 官方文档 | "*…crashes cannot be replayed… set a low AFL_LOOP value… enable AFL_PERSISTENT_RECORD*" | https://aflplus.plus/docs/faq/ |
| 3 | 90/80/20 数值水位 | 官方文档 | "*above 90% is usually fine… even above 20%… below 90% or 80% … take countermeasures*" | https://aflplus.plus/docs/faq/ |
| 4 | 100% vs 覆盖取舍；非 persistent vs persistent 判据 | 官方文档 | "*A 90% stable target… better than a 100% stable target that ignores 10%… Fuzz such targets without persistent mode.*" | https://aflplus.plus/docs/best_practices/ |
| 5 | Windows 在 binary-only 文档中的位置 | 官方文档 | WinAFL "*very good but windows only*"；"*If your target is non-linux, then use unicorn_mode*"；静态重写名单 | https://aflplus.plus/docs/fuzzing_binary-only_targets/ |
| 6 | stability 公式与红色阈值 | 源码（本地） | `afl-fuzz.c:4525 / :4847 / :3096-3097` | 仓库内 |
| 7 | AFL++ 同公式 + var_bytes 导出 | 源码 | `src/afl-fuzz-stats.c:937,511,613-616`；`src/afl-fuzz-ui.c:1466-1470`；`src/afl-fuzz-run.c:534-557` | https://github.com/AFLplusplus/AFLplusplus/tree/stable/src |
| 8 | AFL_PERSISTENT_RECORD 机制 | 源码/文档 | `config.h:137-147`；persistent_mode.md §6 | https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/README.persistent_mode.md |
| 9 | variable 行为四大成因 + AFL_NO_VAR_CHECK | 官方文档（本地） | `afl_docs/status_screen.txt:297-315`、`env_variables.txt:108` | 仓库内 |
| 10 | **fork + 覆盖率共存（fullspeed int3 一次打点、子进程继承）** | 论文 NDSS'21 | "*Breakpoints need only be installed once thanks to the fork-server: child processes inherit…*" | https://www.ndss-symposium.org/wp-content/uploads/ndss2021_6A-3_24334_paper.pdf |
| 11 | WINNIE 弃用 DR/IPT、改 fullspeed；WinAFL 只跑通 26/59 | 论文 NDSS'21 | "*we discard dynamic binary instrumentation in favor of fullspeed fuzzing…*" | 同上 |
| 12 | WINNIE fork 实现（NtCreateUserProcess+CSRSS，310.9 clones/sec，单线程克隆限制） | 论文 NDSS'21 | 见 §V/§X-A | 同上 |
| 13 | **persistent 不适合闭源 Windows（劝退原文）** | 开源项目 README | "*persistent mode is unsuitable for fuzzing closed-source applications, which are predominant on Windows*" | https://github.com/FoRTE-Research/winfuzz （WinFuzz-Winnie README） |
| 14 | **PT 劝退原文** | 开源项目 README | "*The truth is that it sucks so don't use it… buffer overflows easily, leading to poor stability…*" | 同上 |
| 15 | fullspeed = 进程内 Win32 异常处理收集覆盖/崩溃；fork 推荐模式 | 开源项目 README | "*Winnie uses Win32 exception handling… from within its own address space*" | 同上 |
| 16 | WinFuzz 仅 32 位 | 开源项目 README | "*Currently, only 32-bit builds are supported (inline asm).*" | 同上；https://github.com/FoRTE-Research/winfuzz |
| 17 | WinFuzz 原理/性能/闭源内核结论 | 论文 USENIX'23 摘要 | "*combines binary- and library-level hooking… 7–182x*"；"*closed-source nature of its kernel prevents current kernel-based snapshotting techniques from being ported*" | https://www.usenix.org/conference/usenixsecurity23/presentation/stone |
| 18 | WinFuzz hook 用 Detours；spurious/missed crashes 论述 | 论文 USENIX'23 正文 | "*hooks attached with the Detours library*" 等 | 同上 |
| 19 | Winnie 版本兼容性（仅 Win10 21H1/21H2 实测） | 论文 USENIX'23 §RQ4 | "*does not support Windows 8.1 or any version of Windows 11 we tested*" | 同上 |
| 20 | **peafl64 = Windows x64 静态插桩 + WinAFL 兼容** | 开源项目 README | "*Full Windows x64 binaries support… Compatible with WinAFL (headers included) and kAFL*"（TODO: Add tests） | https://github.com/Sentinel-One/peafl64 |
| 21 | pe-afl（BlueHat IL 2019，PE 静态插桩 + WinAFL） | 开源项目 | "*instrumentation on PE binary… for windows user-mode … without source or full symbols*" | https://github.com/wmliang/pe-afl |
| 22 | PeAR：x86/x64 Windows PE + WinAFL 插桩 | 开源项目 + 论文 | "*add AFL++ or WinAFL instrumentation to x64 Linux and x86/x64 Windows binaries*" | https://github.com/pear-labs/PeAR ；https://arxiv.org/pdf/2606.02126 |
| 23 | RetroWrite 需要 ELF/PIC/符号；Dyninst 改写易崩 | 官方文档 | "*still has its symbols and compiled with position independent code*"；"*more often than not binaries crash*" | https://aflplus.plus/docs/fuzzing_binary-only_targets/ |
| 24 | WinAFL `-Y` = syzygy，PE32+PDB，x64 禁用 | 本地源码/文档 | `afl-staticinstr.h:31 #error`；`readme_syzygy.md` "*decompose PE32 binaries with full PDB*" | 仓库内 |
| 25 | PT 按句柄启停（可与 clone 集成的架构缝隙） | 本地源码 | `winaflpt.c:1449-1475 StartProcessIptTracing(child_handle…)` | 仓库内 |
| 26 | PT 调试器依赖 + 缓冲回卷丢覆盖 | 官方文档（本地） | `readme_pt.md:9`（debugger 模式）、`:49-51`（ring buffer wrap） | 仓库内 |
| 27 | PT 故障实例 / VBS 失效 | issue | "*IPT Tracing Error*"；"*Unable to enable Intel PT tracing when VBS is enabled*" | https://github.com/googleprojectzero/winafl/issues/244 ；https://github.com/intelpt/WindowsIntelPT/issues/19 |
| 28 | 本机 PT `err=87` | 本地实测记录 | "*查询 trace 报 err=87…在混合核 CPU 上不稳定*" | 仓库内 `README-FORK.md:108-112` |
| 29 | DR/TinyInst + clone 失败机理 | 本地实验（不重复） | int3 无消费者/DR 后台线程 CoW 不一致 | `fork-poc/A1-TINYINST-VS-DR.md`、`A1-DR-CLONE-FAILURE.md` |
| 30 | fork 引擎 P2 可用 | 本地实测 | 10000 次 0 泄漏、233 clones/sec、本机 19045 | `fork-server/README.md`、`fork-poc/BASELINE.md` |
| 31 | libvips 默认线程数=核数 | 官方文档 | "*by default it will use as many threads as you have CPU cores*" | https://www.libvips.org/API/current/using-threads.html |
| 32 | `VIPS_CONCURRENCY` 环境变量 | 官方 API 文档 | "*the number of threads is set by the environment variable VIPS_CONCURRENCY*" | https://www.libvips.org/API/current/func.concurrency_set.html |
| 33 | operation cache 默认 100、LRU、按参数哈希 | 源码 | `libvips/iofuncs/cache.c`：`static int vips_cache_max = 100` | https://github.com/libvips/libvips/blob/master/libvips/iofuncs/cache.c |
| 34 | foreign load 默认允许缓存（仅 seq 模式 nocache） | 源码 | `libvips/foreign/foreign.c`（`load->nocache = TRUE` 条件） | https://github.com/libvips/libvips/blob/master/libvips/foreign/foreign.c |
| 35 | **OSS-Fuzz libvips harness：单线程 + 关缓存** | 源码 | `vips_concurrency_set(1); vips_cache_set_max(0);` | https://github.com/libvips/libvips/blob/master/fuzz/vips_fuzzer.cc ；https://github.com/google/oss-fuzz/tree/master/projects/libvips |
| 36 | libvips 线程池会破坏 fork（源码注释） | 源码 | "*may break on systems that try to fork() after init*" | https://github.com/libvips/libvips/blob/master/libvips/iofuncs/threadpool.c |
| 37 | 目标导入 WS2_32/DNSAPI/IPHLPAPI 的具体函数 | 本地实测（dumpbin） | WSA*/DnsQuery_A/GetAdaptersAddresses… | `E:\fuzzwork-real\...\libvips-42.dll` |
| 38 | harness 未设并发/缓存/回收（现状缺陷） | 本地源码审阅 | 见 §3.1 | `harness_vips.c` |
| 39 | 覆盖率键为模块相对偏移（ASLR 免疫，E3 有效性依据） | 本地源码 | `winafl.c:318`；`tinyinst_covmap.cpp:134` | 仓库内 |
| 40 | kAFL 支持 Windows 目标（VM 路线存在但重） | 官方教程 | "*Fuzzing on Windows… Windows VM Template*" | https://intellabs.github.io/kAFL/tutorials/windows/index.html |

---

## 附：与交接文档的差异摘要（给接手者）

1. **§3.2"fork 与懒翻译互斥"→ 边界修正**：只否 DR/TinyInst；Winnie fullspeed（进程内 int3）已证明 fork+覆盖率可行 → **P3 的覆盖率来源有了明确、有先例的答案**。
2. **路线 A（静态插桩"只有 32 位"）→ 修正**：syzygy 确实只有 32 位，但 **peafl64 / PeAR 已提供 Windows x64 PE + WinAFL 插桩**；静态路线并未死。
3. **路线 B（DR persistent 先试 1 小时）→ 保留但降级**：预期 ≈17%（引擎无关），只作对照，不作终局。
4. **新最强证据链**：OSS-Fuzz 官方 libvips harness 的三件套设置（我们一个都没做）+ WINNIE/WinFuzz 对 persistent 的系统性否定 + AFL++ 的 20/80/90 阈值 → 决策树见 §5.3。
5. **目标修正**：stability ≈100% → **≥90%（黄线 95%）+ crash 100% 可重放 + bitmap_cvg ≥12.7%**；16.72% 不可接受但可修。
