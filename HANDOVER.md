# 交接文档：WinAFL fork 引擎（RtlCloneUserProcess）

> 交接日期：2026-10-16
> 仓库：`E:\GitCode\winafl`（Brinmon fork，WinAFL 1.17 + AFL 2.43b）
> 前任进度：P2 完成，P3 未开始，**卡在一个必须由你决策的架构问题**

---

## 0. 一句话现状

**fork 引擎（P2）已完全实现并实测通过；但它-fuzz 不出东西——因为插桩与进程克隆在原理上互斥。**

必须先在下面**第 3 节的三个候选方案**中选定一条，才能继续 P3。

---

## 1. 用户要解决的核心问题（原始需求）

| 项 | 内容 |
|---|---|
| **目标** | 对**闭源 Windows 程序**（QQ / 微信 / 钉钉的解析 DLL）做有效的覆盖率引导 fuzzing |
| **痛点** | WinAFL 的 persistent 模式在闭源重目标上 **stability（稳定率）只有 16.72%**，导致数据不可信（假崩溃/漏崩溃） |
| **期望** | stability 提升到 **约 100%**，且吞吐不显著下降 |
| **为什么选 fork** | WINNIE/RtlClone 路线：每个测试用例在独立克隆进程中执行 → 状态天然干净 → stability 应达 100% |

**stability 是什么**（AFL 官方定义）：同一输入重复执行时，覆盖率位图的一致程度。
AFL 源码 `afl-fuzz.c:4847` 把 **< 85% 标红**。AFL 文档原文：
- 多线程目标：*"harmless when the 'stability' metric stays over 90% or so"*
- persistent 模式：*"major dips may signify that the code within `__AFL_LOOP()` is not behaving correctly on subsequent iterations... **most of the fuzzing effort goes to waste**"*

---

## 2. 已完成的工作（P2.1–P2.6 全部实测通过）

### 2.1 交付物

```
E:\GitCode\winafl\fork-server\
├── include\
│   ├── fork_proto.h        (76 行)  协议：magic/version、命令字节、ForkCtl 控制块
│   └── fork_harness.h      (34 行)  harness 唯一 API：FORK_HARNESS(setup, fuzz_one)
├── src\
│   ├── shm.c/.h   (109/49)  共享段：打开 afl-fuzz 的 afl_shm_<id>（覆盖率）
│   │                          + 自建 afl_ctl_<id>（结果/异常码/迭代计数）
│   ├── pipe.c/.h  ( 96/33)  named pipe 客户端（连 \\.\pipe\afl_pipe_<id>）
│   ├── clone.c/.h ( 86/51)  RtlCloneUserProcess 封装、等待/超时、退出码分类
│   └── forkloop.c (202)     fork-server 主循环（fork_harness_main）
├── harness-template\        harness 模板 + build.cmd
├── tests\
│   ├── forkloop_test.c (354)      独立测试驱动（扮演 afl-fuzz）
│   ├── harness_vips_fork.c ( 43)  真实目标 harness（libvips-42.dll）
│   ├── harness_crash_fork.c ( 75) 崩溃注入 harness（4 类异常）
│   └── build.cmd                  一键构建
└── README.md (126)          实现状态 + 实测证据 + 已修正缺陷清单
```

### 2.2 实测结果（可复现）

```bat
cd E:\GitCode\winafl\fork-server\tests
build.cmd                                    :: 需要 VS2022 (vcvars64.bat)
cd build
forkloop_test.exe harness_vips_fork.exe seed.png 10000
```

| 验收项 | 要求 | 实测 | 结论 |
|---|---|---|---|
| P2.1 协议定义 | magic + version | `FORK_PROTO_MAGIC` / `VERSION 2` | ✅ |
| P2.2 共享段 | 双进程读写一致，克隆可穿透写 | cov + ctl 均成功 | ✅ |
| P2.3 pipe 握手 | 与 afl-fuzz 协议一致 | `'P'/'F'/'K'/'C'/'Q'` 完整握手 | ✅ |
| **P2.4 克隆循环** | 10000 次无死锁/泄漏 | **10000 ok / 0 timeout / 0 protocol err；句柄 152→152；工作集 9.5→9.4 MB；233 clones/sec** | ✅ |
| P2.5 崩溃分类 | int3/AV/栈溢出 | `0x80000003` / `0xC0000005` / `0xC00000FD` / exit(1)，父进程存活 | ✅ |
| P2.6 父进程重启 | N 次后重建，计数正确 | 每 10 次 retire，30 次内 3 次重建，句柄 71→71，319 clones/sec | ✅ |

**快速回归**（200 次，约 1 秒）：
```
ok: 200  crashes: 0  timeouts: 0  protocol: 0  handles: 136 -> 136  rate: 217 clones/sec
PASS: fork loop stable (no deadlock, no leak)
```

### 2.3 协议设计（与 afl-fuzz 现有实现完全对齐）

```
harness -> fuzzer : 'P'  就绪，可接受一个测试用例
fuzzer  -> harness: 'F'  开始（输入文件已写好）
harness -> fuzzer : 'K'  干净结束
harness -> fuzzer : 'C'  崩溃，紧跟 1 个 DWORD 异常码
fuzzer  -> harness: 'Q'  退出
```

**关键设计决策**（都在 `fork-poc/DEVELOPMENT-PLAN.md` §1）：
- **D1 复用 afl-fuzz 的共享段**（`afl_shm_<id>`）：`afl-fuzz.c:1502` 已创建，`trace_bits` 已映射
- **D2 复用现有 pipe 协议**：fuzzer 侧零改动
- **D3 harness 自己编译**：fork 逻辑静态链接，无需 DLL 注入
- **D4 快照点在 warmup 之后**：确保库已初始化、无锁持有
- **D6 不加 `NO_SYNCHRONIZE`**：默认路径会排空线程池 + 取 ntdll 锁（安全克隆）
- **父进程不自杀**：`run_target()` 已有"子进程死了就重建"逻辑，自重启会抢同一 pipe

### 2.4 实现时踩过并已修正的 7 个缺陷

见 `fork-server/README.md` 末尾表格，其中最关键两个：

| # | 缺陷 | 根因 |
|---|---|---|
| 2 | 每轮耗时恒等于超时值（看似 hang） | 管道写后 `FlushFileBuffers` 阻塞等对端读，双侧互等 → **死锁**；改为裸 `WriteFile` |
| 5 | 协议方向与 afl-fuzz 相反 | 初版让 harness 等 `'P'`、发 `'F'`；**正确的是 harness 发 `'P'`、fuzzer 回 `'F'`** |

---

## 3. ⚠️ 核心阻塞：插桩与进程克隆**原理上互斥**（A1 实验结论）

### 3.1 实验与证据

详见 `fork-poc/A1-DR-CLONE-FAILURE.md` 与 `A1-TINYINST-VS-DR.md`。

| 引擎 | 无插桩 | 有插桩 + clone | 失败机理 |
|---|---|---|---|
| **DynamoRIO** | PASS | ❌ 普通克隆：`Failed to follow into child process: Child thread not created suspended`；加 `CREATE_SUSPENDED` 后克隆体 `0xC0000005` | DR 后台线程 + 跨进程运行时指针不一致 |
| **TinyInst** | PASS | ❌ 玩具目标**假 PASS**，**真实 libvips 克隆体 `0xC0000005` ×17** | **懒翻译断点无人消费** |
| 无插桩基线 | PASS | ✅ 20/20，213 clones/sec | — |

### 3.2 TinyInst 的决定性证据

```
:: 只插桩 harness（不插桩 libvips）
litecov.exe -instrument_module dr_clone_probe3.exe -- dr_clone_probe3.exe 10 seed3.png
  clone #0..7 bad exit 0x80000003      <- int3 = 未处理断点

:: TinyInst 插桩 libvips-42.dll（真实场景）
litecov.exe -instrument_module libvips-42.dll -- dr_clone_probe3.exe 20 seed3.png
  clone #0..16 bad exit 0xC0000005
```

`0x80000003` = `EXCEPTION_BREAKPOINT`，**根因链**：
1. TinyInst 只在函数入口坐 `0xCC` 断点**预留**插桩（`tinyinst.cpp:168`、`:901`）
2. 命中断点 → 由**调试器线程** `WaitForDebugEvent`（`debugger.cpp:1520`）现场翻译
3. `RtlCloneUserProcess` **只克隆调用线程** → 调试器线程不在克隆体内
4. **任何未提前翻译的 `0xCC` 都变成真 `int3`** → 克隆体终止

**而"未翻译"在 fuzzing 中必然发生**——fuzzing 的目的就是走到新路径，新路径正是尚未翻译的代码。
**结论：fork 与懒翻译在原理上互斥。**

> 玩具目标为什么 PASS：warmup 把 `probe_target` 的所有分支都跑过一遍，
> 克隆体只走已翻译代码。**这是假阳性**，不能外推。

### 3.3 这意味着什么

fork 引擎**本身不产生覆盖率**。当前状态下：
- 没有覆盖率 → `bitmap_cvg` = 0 → **盲 fuzz**
- `stability` 会显示漂亮的 100%，但那是**空洞的**（没有覆盖率就没有一致性可言）

**你的 P3 及 T1/T2/T3 验收标准都要求 `stability ≥95%` 且覆盖率有意义**，
所以必须先解决覆盖率来源。

---

## 4. 三条候选路线（需要你决策）

### 路线 A：静态插桩（唯一能同时满足两个条件）

把覆盖率在**编译期**织入 harness/目标，与进程所有权解耦 → fork 可用。

| 项 | 说明 |
|---|---|
| 现状 | 仓库有 `afl-staticinstr.c/h` + `test_static.cpp`，但**只支持 32 位** |
| 需要 | 为 **x64** 补齐静态插桩（WinAFL 上游有 `-Y` 静态模式概念） |
| 优点 | 干净状态 + 有效覆盖率 **兼得**；无运行时插桩引擎依赖 |
| 代价 | **3–5 天**；需要处理 PE 改写/重定位/x64 指令编码 |
| 风险 | 闭源 DLL **无法**静态插桩（只能插 harness 自身或目标的可写副本）→ **对 QQ/libvips 这种"只 fuzz 第三方 DLL"的场景可能不适用** |

> ⚠️ 关键限制：静态插桩要求**改写目标二进制**。libvips-42.dll 是闭源第三方，
> 你可以改写它的磁盘副本（先复制再插桩），但会引入"插桩后的目标 ≠ 真实目标"的问题。

### 路线 B：改用 DynamoRIO persistent（成本最低，可能直接达标）⭐

**先别做 fork**。A0 实验已有强正面信号：

| 引擎 | 目标 | stability | execs/sec | bitmap_cvg |
|---|---|---|---|---|
| TinyInst persistent | `test.exe`（玩具） | 100.00% | 1878 | 0.09% |
| **DynamoRIO persistent** | **`test.exe`（玩具）** | **98.31%** | **1760** | 0.09% |
| TinyInst persistent | **libvips（真实）** | **16.72%** | 43.83 | **12.73%** |
| TinyInst persistent（iterations=10） | libvips | 17.18% | 18.67 | 12.66% |

**待验证的关键问题**：DR persistent 在**真实 libvips** 上 stability 是多少？
- 若接近 98% → **你的核心目标直接达成，不需要 fork**（成本 1 小时验证）
- 若仍是 ~17% → 说明是 libvips 自身多线程/无状态之外的固有不确定性，
  那 fork 也救不了（需要进一步隔离线程）

**这是性价比最高的一步，强烈建议先做。**

### 路线 C：混合（persistent 主循环 + 周期 fork 重置）

用 persistent 跑 N 次，然后 fork 一次重置状态；或反向。
| 优点 | 折中成本，可能兼顾速度与正确性 |
| 缺点 | 复杂度高；A1 已证明 fork 与插桩互斥，混合仍需先解决覆盖率问题 |

---

## 5. 立即可以执行的验证（路线 B，约 1 小时）

### 5.1 实验材料（全部已就绪）

```
E:\GitCode\winafl\dist\winafl-portable\
  afl-fuzz.exe            WinAFL 1.17
  winafl.dll              DynamoRIO 客户端
  dynamorio\bin64\        DR 运行时
  in_vips\                20 个真实 QQ 图片种子（从 QQ 安装目录提取）
  out_a0\ out_ti\ out_fix\ out_i10\   已有基线数据

E:\fuzzwork-real\harnesses\qq_image_vips\artifacts\
  harness_vips.exe        libvips harness
  harness_vips.pdb        （-target_method 需要）
  libvips-42.dll          ★ 真实闭源目标（20MB，QQ 图片解码器）
  libglib-2.0-0.dll libgobject-2.0-0.dll   libvips 依赖
```

### 5.2 命令（DR persistent，真实目标）

```bat
cd E:\GitCode\winafl\dist\winafl-portable

afl-fuzz.exe -i in_vips -o out_dr_vips -t 20000 ^
  -D dynamorio\bin64 -w winafl.dll ^
  -- -coverage_module libvips-42.dll ^
     -target_module harness_vips.exe -target_method fuzz_one -nargs 1 ^
  -- <harness 目录>\harness_vips.exe @@
```

**注意**：harness 与 libvips DLL 必须在同一目录（或加 `-fuzzer` 参数指定）。

### 5.3 判据

```bat
findstr /C:"stability" out_dr_vips\fuzzer_stats
findstr /C:"bitmap_cvg" out_dr_vips\fuzzer_stats
findstr /C:"execs_per_sec" out_dr_vips\fuzzer_stats
```

| 结果 | 结论 | 下一步 |
|---|---|---|
| stability ≥ 95% 且 bitmap_cvg > 10% | **目标达成，无需 fork** | 把 DR persistent 写进 fuzzkit（P5 简化为换引擎） |
| stability ≥ 95% 但 bitmap_cvg < 1% | 覆盖率不足（DR 对闭源 DLL 的 `-coverage_module` 限制） | 考虑路线 A |
| stability ≈ 17% | 与引擎无关，是目标固有不确定性 | 查多线程/线程池；设 `VIPS_CONCURRENCY=1` 重试 |

---

## 6. 若决定继续 fork（P3 任务清单）

前置：**先解决覆盖率来源**（路线 A 或 B 或 C）。

### P3：afl-fuzz 集成（1.5 天）

| 任务 | 文件 | 要点 |
|---|---|---|
| P3.1 实现 `init_forkserver()` | `afl-fuzz.c:2212`（**当前是 `//not implemented on Windows`**） | 启动 harness + 连 pipe |
| P3.2 新增 `-fork` 模式 | `afl-fuzz.c`（模式标志区约 `:127-130`）+ `CMakeLists.txt` | 与 `-y`/`-D`/`-P` 并列；`--help` 显示 |
| P3.3 `forkserver_run()` 接入 `run_target()` | `afl-fuzz.c:2839` 分支 | 三种结局正确分类（`FAULT_NONE`/`FAULT_CRASH`/`FAULT_TMOUT`） |
| P3.4 覆盖率走共享段 | `afl-fuzz.c:1502`（已有 `afl_shm_<id>`） | `trace_bits` 已映射，fork 天然兼容（**已由 PoC 验证穿透 CoW**） |
| P3.5 传递 fuzzer id | 新增环境变量 `WINAFL_FORK_ID` | harness 靠它找 pipe 和共享段（`fork_proto.h` 已定义） |

**关键约束**（`afl-fuzz.c:2850-2860` 的现有结构）：
```c
static u8 run_target(char** argv, u32 timeout) {
  if (dll_run_target_ptr) return dll_run_target_ptr(...);
  if (use_intelpt) return run_target_pt(argv, timeout);
  if (use_tinyinst) return tinyinst_run(argv, timeout);
  // ← -fork 分支加在这里
}
```

---

## 7. 环境与工具链（避免重复踩坑）

| 项 | 值 |
|---|---|
| 编译器 | `D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat` |
| 构建 fork-server | `cd fork-server\tests && build.cmd`（会调 vcvars64） |
| **PowerShell 5.1 GBK 陷阱** | `.ps1` 无 BOM 时中文会破坏解析 → **脚本保持 ASCII** |
| **cmd 引号陷阱** | `cmd /c "<含引号的命令>"` 会吞内层引号 → 用临时脚本文件 |
| **Python 写脚本陷阱** | `\n` 在 heredoc 里会变成真换行；用 edit 工具而非 Python 注入代码 |
| **管道继承陷阱** | 长命分离进程必须清 `HANDLE_FLAG_INHERIT`，否则 `cmd \| tail` 会阻塞到进程结束 |
| Windows 弹窗 | 所有 spawn 必须加 `CREATE_NO_WINDOW`（见 fuzzkit 的 FX-001） |

---

## 8. 相关文档索引

| 文档 | 内容 | 优先级 |
|---|---|---|
| `fork-poc/DEVELOPMENT-PLAN.md` | 完整开发计划（架构、协议、测试矩阵、风险） | ★★★ |
| `fork-poc/BASELINE.md` | persistent 基线数据（16.72%） | ★★★ |
| `fork-poc/A1-DR-CLONE-FAILURE.md` | DR + clone 失败实验（复现命令齐全） | ★★★ |
| `fork-poc/A1-TINYINST-VS-DR.md` | TinyInst vs DR 对照 + 失败机理 | ★★★ |
| `fork-server/README.md` | P2 实现状态、实测证据、7 个已修正缺陷 | ★★ |
| `fork-poc/README.md` | 早期 PoC（克隆/共享段/坑位验证） | ★ |
| `fork-poc/clone_poc.c` `shared_mem_poc.c` `pitfall_test.c` | 三个 PoC 源码 | ★ |

**另一条并行线**：`E:\GitCode\fuzzkit`（Rust CLI，用于编排 fuzzing 任务 + 统一数据）。
其 P5 任务需要支持 `instrumentation.mode = "fork"`，但**等覆盖率方案定了再动**。

---

## 9. 给接手者的建议顺序

```
1. 【1 小时】跑第 5 节的 DR persistent 真实目标实验   ← 性价比最高，可能直接收工
2. 【决策】 根据结果选路线 A/B/C，并更新 DEVELOPMENT-PLAN.md
3. 【按路线推进】P3 → T1（test.exe）→ T2（crypt32）→ T3（libvips，核心验收）
4. 【收尾】T4 长跑 → P4 调优 → P5 fuzzkit 接入 → P6 对比报告
```

**最重要的提醒**：
- 不要相信玩具目标（`test.exe`）的 PASS —— A1 已证明 TinyInst 在玩具上是**假阳性**
- 任何 stability 数字都要同时看 `bitmap_cvg`；**`bitmap_cvg` = 0 时 stability 无意义**
- fork 引擎本身已完全可用（P2 实测 10000 次无泄漏），**问题只在覆盖率来源**
