# 调研任务：闭源 Windows DLL fuzzing 的 stability 问题与可行技术路线

> 交付对象：接手调研的 agent
> 背景仓库：`E:\GitCode\winafl`（WinAFL fork，含已完成的 fork 引擎）
> 交接文档：`E:\GitCode\winafl\HANDOVER.md`（先读它了解工程现状）
> 本文只描述**调研任务**，不含实现任务。

---

## 0. 一句话

我们要判断：**在闭源 Windows DLL（QQ libvips-42.dll）上做覆盖率引导 fuzzing，
到底是"必须实现真 fork"，还是"换插桩引擎/调参就能达标"？**
请给出有权威依据、可执行、有取舍的结论。

---

## 1. 背景：用户要解决的核心问题

| 项 | 内容 |
|---|---|
| **目标** | 对**闭源 Windows 程序**（QQ / 微信 / 钉钉的解析 DLL）做有效的、覆盖率引导的 fuzzing |
| **现象** | WinAFL + TinyInst **persistent 模式**在真实目标 `libvips-42.dll` 上 **stability = 16.72%** |
| **判据** | 期望 stability 提升到 **约 100%**，且吞吐不显著下降 |
| **已投入** | 已实现完整 `RtlCloneUserProcess` fork 引擎（P2 完成，10000 次克隆 0 泄漏、233 clones/sec） |

### 1.1 已实测的基线数据（必须先看，避免重复劳动）

文件：`E:\GitCode\winafl\fork-poc\BASELINE.md`、`E:\GitCode\winafl\dist\winafl-portable\out_*/fuzzer_stats`

| 引擎 / 模式 | 目标 | stability | execs/sec | bitmap_cvg | paths |
|---|---|---|---|---|---|
| TinyInst persistent（iterations=2000） | `test.exe`（玩具） | **100.00%** | 1878 | 0.09% | 12 |
| **DynamoRIO persistent** | `test.exe`（玩具） | **98.31%** | 1760 | 0.09% | 12 |
| **TinyInst persistent** | **libvips（真实）** | **16.72%** | 43.83 | **12.73%** | 55 |
| TinyInst persistent（iterations=10） | libvips | 17.18% | 18.67 | 12.66% | 38 |
| fork + 无插桩 | libvips | （无覆盖率，故无意义） | 217 clones/sec | **0** | — |

**关键观察**：玩具目标 98–100%，真实目标 16.72% → **问题与目标相关，不是引擎本身**。

### 1.2 已实测的阻塞：插桩与进程克隆互斥

文件：`E:\GitCode\winafl\fork-poc\A1-DR-CLONE-FAILURE.md`、`A1-TINYINST-VS-DR.md`

| 引擎 | 有插桩 + `RtlCloneUserProcess` | 失败机理（已用源码定位） |
|---|---|---|
| DynamoRIO | ❌ 普通克隆：`Failed to follow into child process: Child thread not created suspended`；加 `CREATE_SUSPENDED`：克隆体 `0xC0000005` | DR 是注入 DLL + 后台线程；CoW 快照里指向父进程堆/句柄/线程对象的指针不一致 |
| TinyInst | ❌ 玩具目标**假 PASS**；真实 libvips 克隆体 `0xC0000005` ×17 | `0xCC` 断点由**调试器线程**消费（`WaitForDebugEvent`），而克隆只复制调用线程 → 未翻译断点变真 `int3`（`0x80000003`） |

**推理**：fuzzing 的目的是走到**新代码路径**，而新路径必然尚未被 TinyInst 懒翻译 →
**fork 与懒翻译在原理上互斥**。

---

## 2. 调研任务（三个问题，按优先级）

### ❗ 问题 1（最高优先级）：stability 到底影响什么？它是不是必须追求 100%？

**已知的权威片段**（我已取到，请补充更完整的证据与边界）：

AFL++ 官方 FAQ（https://aflplus.plus/docs/faq/）原文：
> *"The more 'unstable' edges there are, the harder it is for AFL++ to identify valid new paths."*
> *"If you fuzz in persistent mode... a large number of unstable edges can mean that the target keeps internal state and therefore **it is possible that crashes cannot be replayed**. In such a case do either not fuzz in persistent mode..."*
> *"A value above 90% is usually fine and a value above 80% is also still ok, and **even a value above 20% can still result in successful finds of bugs**. However, it is recommended that for values below 90% or 80% you should take countermeasures to improve stability."*

AFL++ best practices（https://aflplus.plus/docs/best_practices/）原文：
> *"For fuzzing, a 100% stable target that covers all edges is the best case. A 90% stable target that covers all edges is, however, **better than a 100% stable target that ignores 10% of the edges**."*
> *"Note that this is only useful for non-persistent targets! **If a persistent target is unstable whereas when run non-persistent is fine, then this means that the target is keeping internal state, which is bad for fuzzing. Fuzz such targets without persistent mode.**"*

**请调研并回答**：
1. stability 低的具体后果有哪些？（假阳性崩溃 / 漏报崩溃 / 队列膨胀 / 路径识别困难 / 其他）
2. **16.72% 在实际使用中是否"可用"？** 官方说 >20% 仍可能成功，那 16.72% 处于什么位置？有无实测/案例研究支持"低 stability 仍能挖到 bug"？
3. **"100%" 是否是合理目标？** 对多线程/依赖系统状态的真实目标，业界实际达到的水平是多少？
4. stability 与 **crash 可复现性**的定量关系是什么？（我们最关心：低 stability 下找到的 crash 能否重放）
5. AFL/AFL++/其他 fuzzer 的 stability 计算方式差异（尤其 AFL 2.43b(AFL++ 前身) vs AFL++ 新版），以及 `var_bytes` 的含义。

**要求的证据标准**：优先官方文档、源码、论文；给出可点击链接与原文引用。

---

### ❗ 问题 2：闭源 Windows 二进制 fuzzing 的覆盖率采集，有哪些**与进程所有权解耦**的方案？

背景：我们的 fork 引擎需要一个**不依赖"在目标进程内维持运行时状态"**的覆盖率来源。

**已排除**：DynamoRIO（跨进程运行时）、TinyInst（懒翻译断点依赖调试器线程）。

**请调研以下候选（每个都要给：原理 / 是否可与进程克隆共存 / 实际性能 / 成熟度 / 对闭源 DLL 的适用性 / 链接）**：

| 候选 | 已知信息 | 需要确认 |
|---|---|---|
| **WinAFL `-Y` 静态插桩** | 仓库有 `afl-staticinstr.c/h`、`test_static.cpp`，但**只支持 32 位** | x64 是否已有实现/分支？改写闭源 DLL 的可行性（重定位、校验和、签名）？ |
| **Intel PT（`-P`）** | WinAFL 已支持；我们的 A0 实测在 Alder Lake 上 `err=87`（混合核） | 是否与 clone 兼容（PT 是硬件追踪，缓冲区在内核）？换 Intel 非混合核 CPU 是否可用？ |
| **静态二进制改写工具**（Zafl / RetroWrite / Dyninst / E9AFL） | AFL++ 文档提到用于 binary-only 目标 | 是否支持 **Windows PE/x64**？（多为 ELF/Linux） |
| **驱动/内核级快照**（如 Linux 的 snapshot fuzzing 思路） | Linux 有 KVM/内核快照 | Windows 闭源内核是否可行？有无开源实现？ |
| **目标内快照（target-embedded snapshotting, WinFuzz）** | USENIX'23，7–182x 提速，**但只支持 32 位** | x64 移植工作量？其 hook 机制与闭源 DLL 的兼容性？ |
| **硬件断点/单步 + 自建边表** | 无 | 可行性、性能、与 clone 的兼容性？ |

**额外问题**：
- AFL++ 的 `fuzzing_binary-only_targets` 文档（https://aflplus.plus/docs/fuzzing_binary-only_targets/）里，**Windows 目标的推荐方案**是什么？原文说 DynamoRIO *"very good but windows only"*，但它是我们的被排除项——有无替代？
- 是否存在"**插桩一次、快照复用**"的方案（即插桩状态可以随 fork 一起复制）？

---

### ❗ 问题 3：libvips 的 16.72% 究竟由什么造成？（定位根因，可能无需 fork）

**假设 A**：persistent 模式的状态污染（WINNIE 论文的论点）
**假设 B**：libvips 自身多线程（GLib 线程池 / vips 并发）
**假设 C**：libvips 有网络/系统依赖引入不确定性（**实测它导入了 `WS2_32.dll`、`DNSAPI.dll`、`IPHLPAPI.DLL`**）

**请调研并回答**：
1. libvips（vips 8.x / glib 2.x）有哪些**已知的不确定性来源**？`VIPS_CONCURRENCY`、`VIPS_DISC_THRESHOLD`、`vips_shutdown`、`vips_cache_set_max` 等环境变量/API 能否显著改善确定性？
2. 有无**公开案例**：vips/libvips 作为 fuzz 目标的稳定性经验（OSS-Fuzz 上 libvips 的表现）？
3. 定量区分 A/B/C 的**实验设计**（要可执行、成本可控）：例如
   - 设 `VIPS_CONCURRENCY=1` 后 stability 是否上升？
   - 非 persistent（每次新进程）跑同一输入 N 次，stability 是多少？
   - 只插桩 harness 不插桩 libvips，stability 是多少？
4. 如果根因是 B/C，是否**不需要 fork** 就能达标？

---

## 3. 交付物要求

### 3.1 一份报告（Markdown），必须包含

1. **三个问题的结论**（每个都要有明确判断，不要"可能""也许"）
2. **证据表**：结论 → 来源（官方文档/源码/论文/实测）→ 原文引用 → 链接
3. **取舍矩阵**：可选技术路线 × 维度（stability 预期 / 吞吐 / 改造成本 / 对闭源 DLL 适用性 / 成熟度 / 风险）
4. **推荐路线 + 理由**，并明确说明**不推荐路线的否决理由**
5. **可执行的验证步骤**（命令行级别，能直接跑）
6. **不确定性清单**：哪些结论证据不足，需要进一步实验

### 3.2 判据（决定后续工程方向）

请明确回答：**为了达成"stability 接近 100%"，以下哪条是正解？**

| 选项 | 含义 |
|---|---|
| **B1** | 换插桩引擎（如 DynamoRIO persistent）即可达标 → 不需要 fork |
| **B2** | 调参/环境隔离（单线程、关闭缓存等）即可达标 → 不需要 fork，也不需要换引擎 |
| **B3** | 必须实现"覆盖率与进程解耦"（静态插桩 / 其他） + fork → 需要继续 P3 |
| **B4** | 16.72% 实际上够用（官方说 >20% 仍可成功，16.72% 接近）→ **目标本身可能需要修正** |
| **B5** | 其他（请说明） |

**特别关注 B4**：如果权威证据支持"16.72% 在实践中仍能有效挖 bug"，那么**用户的原始目标（stability 100%）可能需要被重新定义**——请给出明确建议。

---

## 4. 背景材料索引（调研前请先读）

### 4.1 必须读（工程现状）

| 文件 | 内容 |
|---|---|
| `E:\GitCode\winafl\HANDOVER.md` | ★ 完整交接：核心问题、已完成工作、阻塞、三条候选路线、P3 任务清单 |
| `E:\GitCode\winafl\fork-poc\BASELINE.md` | persistent 基线（16.72%）的完整数据与复现命令 |
| `E:\GitCode\winafl\fork-poc\A1-TINYINST-VS-DR.md` | ★ TinyInst vs DR 对照实验 + 失败机理（含源码行号） |
| `E:\GitCode\winafl\fork-poc\A1-DR-CLONE-FAILURE.md` | DR + clone 失败实验（复现命令齐全） |
| `E:\GitCode\winafl\fork-poc\DEVELOPMENT-PLAN.md` | 原开发计划（架构、协议、测试矩阵） |
| `E:\GitCode\winafl\fork-server\README.md` | fork 引擎实现状态与实测证据 |

### 4.2 环境（如需实测）

```
E:\GitCode\winafl\dist\winafl-portable\
  afl-fuzz.exe / winafl.dll / dynamorio\bin64\
  in_vips\            20 个真实 QQ 图片种子
  out_a0\ out_ti\ out_fix\ out_i10\   已有基线 fuzzer_stats

E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\
  harness_vips.exe + .pdb
  libvips-42.dll（20MB 闭源目标）
  libglib-2.0-0.dll / libgobject-2.0-0.dll

E:\GitCode\winafl\fork-server\tests\build\
  forkloop_test.exe       fork 引擎测试驱动
  harness_vips_fork.exe   fork 版 harness
```

### 4.3 已收集的权威原文（可直接引用）

**AFL++ FAQ**（https://aflplus.plus/docs/faq/）
> *"Stability is measured by how many percent of the edges in the target are 'stable'... The more 'unstable' edges there are, the harder it is for AFL++ to identify valid new paths."*
> *"If you fuzz in persistent mode... a large number of unstable edges can mean that the target keeps internal state and therefore it is possible that crashes cannot be replayed."*
> *"A value above 90% is usually fine and a value above 80% is also still ok, and even a value above 20% can still result in successful finds of bugs. However, it is recommended that for values below 90% or 80% you should take countermeasures to improve stability."*

**AFL++ Best Practices**（https://aflplus.plus/docs/best_practices/）
> *"For fuzzing, a 100% stable target that covers all edges is the best case. A 90% stable target that covers all edges is, however, better than a 100% stable target that ignores 10% of the edges."*
> *"Note that this is only useful for non-persistent targets! If a persistent target is unstable whereas when run non-persistent is fine, then this means that the target is keeping internal state, which is bad for fuzzing. Fuzz such targets without persistent mode."*

**AFL++ binary-only targets**（https://aflplus.plus/docs/fuzzing_binary-only_targets/）
> DynamoRIO *"very good but windows only"*；*"The big issue with DynamoRIO (and therefore Pintool, too) is speed. DynamoRIO has a speed decrease of 98-99%"*
> *"If your target is non-linux, then use unicorn_mode."*

**AFL 源码**（`E:\GitCode\winafl\afl-fuzz.c:4847`）
```c
SAYF(" stability : %s%-9s " bSTG bV "\n", (stab_ratio < 85 && var_byte_count > 40)
     ? cLRD : ((queued_variable && (!persistent_mode || var_byte_count > 20))
     ? cMGN : cRST), tmp);
```
→ AFL 把 **< 85% 标红**。

**USENIX Security '23 "No Linux, No Problem"**（target-embedded snapshotting）
> *"in-memory looping... does not reset process state between test cases—overwhelming fuzzers with false-positive crashes caused by polluted heap and global state... leads to both spurious and missed crashes"*
> WinFuzz vs Winnie vs WinAFL 吞吐：**7.0x / 182.7x 更快**；但**仅支持 32 位**

**WINNIE (NDSS'21)** README 原文
> *"Persistent mode is unsuitable for fuzzing closed-source applications... Unless the target function behaves like a pure function (i.e., no side effects), the program state will change each iteration, eventually leading to divergence."*

---

## 5. 约束与注意事项

| 约束 | 说明 |
|---|---|
| **不要重复已做过的实验** | 见 §1.1/§1.2 的实测数据；如需复现，用 §4.2 的材料 |
| **不要轻信玩具目标** | A1 已证明 TinyInst 在 `test.exe` 上 PASS 是**假阳性**（warmup 已翻译全部代码） |
| **stability 必须与 bitmap_cvg 一起看** | `bitmap_cvg`=0 时 stability=100% 是**空洞的** |
| **区分"引擎问题"与"目标问题"** | 玩具 98–100% vs 真实 16.72% → 很可能是**目标特性**，请在问题 3 中重点论证 |
| 环境 | Win10 19045 / x64 / VS2022 / Node 22；Windows + git-bash |
| 报告语言 | 中文（技术名词保留英文） |

---

## 6. 期望的最终输出

一份 Markdown 报告，能回答：

1. **stability 16.72% 是否可接受？**（是/否 + 依据）
2. **stability 100% 是否是正确目标？**（是/否 + 建议的目标值）
3. **达成目标的正确技术路线是什么？**（B1–B5 中的哪一个 + 理由）
4. **下一步该做什么实验/改动？**（可执行步骤）
5. **哪些结论证据不足？**（不确定性清单）
