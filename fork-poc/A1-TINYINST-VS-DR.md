# A1 修正：TinyInst vs DynamoRIO — 实测对照

> 日期：2026-10-16 · 环境：Win10 19045 / x64 / VS2022
> 起因：早前仅实测 DynamoRIO 就把结论推广为"方案 A 不可行"，属未经测量的外推。
> 本文补齐 TinyInst 实测并分离两者的失败机理。

## 一、结论

**两个引擎都无法支持 fork，但失败机理不同**，且 TinyInst 在**玩具目标上会给出假阳性**：

| 引擎 | 玩具目标（toy） | 真实目标（libvips-42.dll） | 失败特征 |
|---|---|---|---|
| 无插桩（基线） | PASS 20/20 | **PASS 20/20**（213 clones/sec） | — |
| **DynamoRIO** | 拒跟进 / `0xC0000005` | — | 跨进程运行时 + 后台线程 |
| **TinyInst** | **PASS 3/3**（假阳性！） | **`0xC0000005` ×17** | 懒翻译断点无人消费 |

**关键教训**：TinyInst 在 toy 目标上的 PASS **不能外推**——真实 DLL 一旦触发未翻译的新代码路径，克隆体立刻崩溃。

## 二、TinyInst 实测证据

### 2.1 玩具目标（会误判为"可用"）

```
litecov.exe -- dr_clone_probe2.exe 3 2000 normal
  [17564] CLONE: branch taken, calling instrumented target
  [17564] CLONE: instrumented target returned, exiting
  created: 3  ok(exit0): 3  timeouts: 0
  RESULT: PASS
```

克隆体确实执行了插桩目标并正常退出。**若只看这一步，会误判 TinyInst 可行。**

### 2.2 真实目标（暴露真问题）

测试：克隆体内解析一张真实 PNG，经 `libvips-42.dll`（20MB 闭源）——
即模糊测试的真实场景（深且数据相关的控制流）。

```
:: 基线：无插桩
dr_clone_probe3.exe 20 seed3.png
  created: 20  ok(parse): 20  bad exit: 0  timeouts: 0
  elapsed: 94ms (213 clones/sec)
  RESULT: PASS

:: TinyInst 插桩 libvips-42.dll
litecov.exe -instrument_module libvips-42.dll -- dr_clone_probe3.exe 20 seed3.png
  clone #0..16 bad exit 0xC0000005     <- 每一个都 AV 崩溃
```

### 2.3 控制实验：定位根因

```
:: 只插桩 harness（不插桩 libvips）
litecov.exe -instrument_module dr_clone_probe3.exe -- dr_clone_probe3.exe 10 seed3.png
  clone #0..7 bad exit 0x80000003      <- int3 = 未处理断点
```

`0x80000003` = `EXCEPTION_BREAKPOINT`。这是决定性证据：
**克隆体撞上了 TinyInst 留下的 `0xCC` 断点，而消费断点的调试器不存在。**

## 三、机理：为什么两个引擎都不行

### TinyInst：断点驱动的懒翻译

TinyInst 的工作模型（源码证据）：

1. `InstrumentModule`（`tinyinst.cpp:901`）只**预留**插桩代码缓冲区
   （`RemoteAllocateNear` 分配到目标进程内），并遍历函数入口坐下 `Breakpoint`
   （`tinyinst.cpp:168`、`InitializeRemoteCode` 附近）；
2. 命中 `0xCC` → 由**调试器循环** `WaitForDebugEvent`（`debugger.cpp:1520`）
   收到 → 交给 `HandleBreakpoint`（`tinyinst.cpp:279`）→ 现场翻译该基本块；
3. 覆盖率由内联 `inc byte ptr[rax]` 写 `coverage_buffer_remote`（`litecov.cpp:144`）。

**第 2 步是致命的**：`RtlCloneUserProcess` 只克隆**调用线程**
（`fork-poc/README.md:41` 自述），调试器线程不在克隆体内 → 任何未提前翻译的
`0xCC` 都成为真 `int3` → 克隆体以 `0x80000003` 终止。

而"未翻译"在真实目标上**必然发生**：模糊测试的整个目的就是让输入走到**新路径**，
而新路径正是尚未翻译的代码。**fork 与懒翻译在原理上互斥。**

（玩具目标为什么 PASS：`dr_clone_probe2` 在 warmup 里把 `probe_target`
的所有分支都跑过一遍，克隆体只走已翻译代码，因此侥幸成功。这正好说明
**玩具目标的 PASS 是假阳性**。）

### DynamoRIO：跨进程运行时

DR 是**注入 DLL** 形态，目标进程内有 DR 运行时和它的后台线程。DR 的 follower
要求子进程主线程 `CREATE_SUSPENDED` 创建以便接管；`RtlCloneUserProcess` 默认不满足，
DR 直接终止应用；显式加 `CREATE_SUSPENDED` 后 DR 接受，但克隆体的 CoW 快照里
DR 运行时数据结构（指向父进程堆/句柄表/线程对象的指针）不一致 → `0xC0000005`。

### 共同根因

**两个引擎都是"跨进程/多线程 的插桩运行时"，而 fork 是"单线程 + CoW 快照"。
插桩方必须拥有并持续管理目标进程，fork 把这个所有权交给了 harness。**

## 四、顺带确认的基线（真实目标，TinyInst persistent）

用正确口径（插桩 `libvips-42.dll`）复现基线，与 `BASELINE.md` 高度一致：

| 指标 | BASELINE.md | 本次复现 |
|---|---|---|
| stability | 16.72% | **16.65%** |
| execs/sec | 43.83 | 48.36 |
| bitmap_cvg | 12.73% | 12.72% |
| paths_total | 55 | 52 |

同时确认 TinyInst persistent 在 `test.exe` 上 stability = **100.00%**（1878 eps）——
即：**stability 低是"闭源重目标 + persistent 状态污染"特有的，不是 TinyInst 本身的问题。**

## 五、对核心需求的影响

| 需求 | 状态 |
|---|---|
| `RtlCloneUserProcess` fork 引擎 | ✅ 已实现并独立验证（P2，10000 次 0 泄漏，233 clones/sec） |
| 效率不显著下降 | ✅ 克隆 213–233 clones/sec（插桩开销另计） |
| **stability 100%** | ❌ **无插桩引擎可与之共存** → 无法在"有覆盖率"的前提下达成 |

**stability 100% 需要两个条件同时成立**：干净状态（fork 给）**且**有效覆盖率（插桩给）。
实测证明这两个条件在当前 WinAFL 架构下互斥。

## 六、可选的出路（需你决策）

1. **静态插桩（原方案 C）**：把覆盖率在**编译期**织入 harness/目标，
   与进程所有权解耦 → fork 可用。x64 需自建（`afl-staticinstr.h` 仅 32 位）。
2. **接受 DR persistent 的现实**：A0 已实测 DR persistent 在 `test.exe` 上
   stability **98.31%**、1760 eps。若在 libvips 上也能接近该水平，
   则"16.72% → ~98%"的目标**不用 fork 就能达成**（代价：非 100%）。
3. **混合**：persistent 跑主循环 + 周期性 fork 重置（用 fork 修 persistent 的状态污染，
   而不是替代它）。

建议先做 **2 的实测**（成本最低、A0 已有 98.31% 的正面信号），它可能直接满足
"stability 接近 100%"的实质目标。
