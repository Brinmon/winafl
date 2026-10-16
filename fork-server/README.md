# fork-server — 实现状态与实测证据

> 引擎核心（P2.1–P2.6）已完成并实测通过。
> 目标：以 `RtlCloneUserProcess` 实现真 fork 引擎，替代 persistent 模式
> （闭源目标下 stability 基线仅 16.72%）。
> 计划见 `../fork-poc/DEVELOPMENT-PLAN.md`。

构建与测试（x64）：

```bat
cd tests
build.cmd                  :: 需要 VS2022 (vcvars64)
cd build
forkloop_test.exe harness_vips_fork.exe  seed.png 10000          :: P2.4
forkloop_test.exe harness_crash_fork.exe seed.png 20 1           :: P2.5 int3
forkloop_test.exe harness_crash_fork.exe seed.png 20 2           :: P2.5 AV
forkloop_test.exe harness_crash_fork.exe seed.png 20 3           :: P2.5 stack overflow
forkloop_test.exe harness_crash_fork.exe seed.png 20 4           :: P2.5 exit(1)
set FORK_TEST_RESTART_AFTER=10
forkloop_test.exe harness_crash_fork.exe seed.png 30 0           :: P2.6 retire/recreate
```

## 模块

| 文件 | 职责 |
|---|---|
| `include/fork_proto.h` | 协议定义（命令字节、控制块、magic/version） |
| `include/fork_harness.h` | harness 侧唯一 API（`FORK_HARNESS(setup, fuzz_one)` 展开出 `main`） |
| `src/shm.c` | 共享段：打开 afl-fuzz 的 `afl_shm_<id>`（覆盖率）+ 自建 `afl_ctl_<id>`（结果） |
| `src/pipe.c` | named pipe 客户端（连 afl-fuzz 的 `\\.\pipe\afl_pipe_<id>`） |
| `src/clone.c` | `RtlCloneUserProcess` 封装、等待/超时、退出码分类 |
| `src/forkloop.c` | fork-server 主循环（`fork_harness_main`） |
| `tests/forkloop_test.c` | 独立测试驱动：扮演 afl-fuzz（建 pipe + cov map、发 `'F'`、收 `'K'`/`'C'`、P2.6 重建） |
| `tests/harness_vips_fork.c` | 真实目标 harness（libvips-42.dll 图片解析） |
| `tests/harness_crash_fork.c` | P2.5 崩溃注入 harness（4 类） |

## 协议（与 afl-fuzz 现有 `run_target()` 完全对齐，fuzzer 侧零改动）

```
harness -> fuzzer : 'P'  就绪，可接受一个测试用例
fuzzer  -> harness: 'F'  开始（输入文件已写好）
harness -> fuzzer : 'K'  干净结束
harness -> fuzzer : 'C'  崩溃，紧跟 1 个 DWORD 异常码
fuzzer  -> harness: 'Q'  退出
```

主循环（`forkloop.c`）：

```
connect pipe -> open shared sections -> setup()   <<< 快照点（此后不持锁）
loop:
  dispatch dump 快照           (写 'P')  <- 指标 dump 由 harness 自身完成
  wait 'F' / 'Q'
  RtlCloneUserProcess
    child : fuzz_one(input) -> 记 outcome -> ExitProcess(0)
    parent: 等待/分类 -> 回复 'K' 或 'C'+code
  每 N 次: 干净退出，由 afl-fuzz 重建（P2.6）
```

关键机制：
- **父进程不重启自身**：afl-fuzz 的 `run_target()` 已有
  `if (!is_child_running()) { destroy_target_process(); create_target_process(); }`，
  且其 named pipe 是单实例 —— 自重启会与 fuzzer 抢同一 pipe。干净退出即把重建交给 fuzzer
  （与 TinyInst `dr_exit_process` 同一契约）。P2.6 按此语义在测试驱动中实现并验证。
- **pipe 写不做 `FlushFileBuffers`**：命名管道上 flush 会阻塞到对端读取，双侧 flush 即死锁。
  afl-fuzz 自己的 `WriteCommandToPipe` 也是裸 `WriteFile`（字节模式管道即时投递）。

## 实测结果（Win10 19045 / x64 / VS2022）

| 任务 | 验收 | 实测 | 结论 |
|---|---|---|---|
| P2.1 协议定义 | magic + version | `FORK_PROTO_MAGIC` / `VERSION 2` | ✅ |
| P2.2 共享段 | 双进程读写一致 | cov + ctl 双段打开成功，克隆可穿透写 | ✅ |
| P2.3 pipe 客户端 | 与 afl-fuzz 握手 | 与测试驱动 `'P'/'F'/'K'/'C'/'Q'` 完整握手 | ✅ |
| P2.4 克隆循环 | 10000 次无死锁/泄漏 | **10000 ok / 0 timeout / 0 protocol err**；句柄 152→152；工作集 9.5→9.4 MB；**233 clones/sec** | ✅ |
| P2.5 崩溃分类 | int3/AV/栈溢出三类 | int3 `0x80000003`、AV `0xC0000005`、栈溢出 `0xC00000FD`、exit(1) `0x00000001`；父进程存活；句柄恒定 | ✅ |
| P2.6 父进程重启 | N 次后重启，计数正确 | 每 10 次 retire：30 次内 **3 次 retire/recreate**，0 timeout，句柄 71→71，319 clones/sec | ✅ |

原始输出（节选）：

```
== P2.4: 10000 次克隆 + 真实 libvips 解析 ==
  ok        : 10000
  crashes   : 0
  timeouts  : 0   <- deadlock indicator (must be 0)
  protocol  : 0 errors
  handles   : 152 -> 152   <- leak indicator (must be equal)
  work set  : 9.5 MB -> 9.4 MB
  rate      : 233 clones/sec
PASS: fork loop stable (no deadlock, no leak)

== P2.5: mode=1/2/3/4 ==
  crash at iter N: exception=0x80000003   (int3)
  crash at iter N: exception=0xC0000005   (access violation)
  crash at iter N: exception=0xC00000FD   (stack overflow)
  crash at iter N: exception=0x00000001   (exit(1))

== P2.6: FORK_TEST_RESTART_AFTER=10, 30 iters ==
  forkserver retired (exit=0x00000000) -> recreated (#1..#3)
  ok: 30   timeouts: 0   handles: 71 -> 71
```

## 实现过程中修正的缺陷（便于回归）

| # | 缺陷 | 根因 | 修正 |
|---|---|---|---|
| 1 | harness 启动即崩（无输出） | `fork_shm_open_cov` 里 `memset(s)` 把 `fork_shm_open_ctl` 已填好的 `s->ctl` 清零 → 调用方空指针解引用 | 把 `memset` 收敛到 `fork_shm_open` 一处；两个 helper 只碰自己的字段 |
| 2 | 每轮耗时恒等于超时值（看似 hang） | 管道写后 `FlushFileBuffers` 阻塞等对端读取，双侧互相等待 → 死锁 | 两侧都改为裸 `WriteFile`（与 afl-fuzz 一致） |
| 3 | 测试驱动看不到进度 | stdout 重定向后块缓冲 | `setvbuf(_IONBF)` |
| 4 | 句柄"泄漏"假阳性（47→152） | 基线采样早于 harness 的 `setup()`（libvips 加载） | 先跑一轮预热再采基线 |
| 5 | 协议方向不一致 | 初版让 harness 等 `'P'`、发 `'F'`，与 afl-fuzz 现有实现相反 | 翻正为 harness 发 `'P'`、fuzzer 回 `'F'`（D2：复用现有协议，fuzzer 侧零改动） |
| 6 | 栈溢出未触发（判为干净） | 递归被优化成尾调用/循环 | `#pragma optimize("", off)` + 每页触碰 16KB 栈帧 |
| 7 | retire 后每轮耗约 20s | 阻塞在 pipe 读上等满超时才发现进程已退出 | 照 afl-fuzz 的 `is_child_running()`，**阻塞前**先探活 |

## 下一步（P3 前需定的架构决策）

`-fork` 模式的**覆盖率来源**。fork 引擎本身不产生覆盖率：闭源 DLL 无法静态插桩，
而 `run_target()` 在 fork 分支下不再走 DynamoRIO/TinyInst。三种候选：

| 方案 | 机制 | 代价 |
|---|---|---|
| **A. harness 运行在现有 DynamoRIO/TinyInst 下** | 插桩先于克隆；克隆体执行被插桩的 DLL 代码，覆盖率写入共享段（已证明穿透 CoW） | 需实测 DR 自身状态能否安全穿过 `RtlCloneUserProcess`（后台线程/代码缓存） |
| **B. harness 侧自建插桩** | 按 DEVELOPMENT-PLAN §D7「覆盖率自建」，在 harness 内实现覆盖率采集 | 需自建边表；闭源 DLL 只能用采样式（`fork-poc/cov_stability.c` 已验证但精度低） |
| **C. 仅打通管道，覆盖率依赖目标自带** | P3 只交付模式/握手/fuzzer_stats，覆盖率质量留待 T3 由仪器层补 | T3 的 `bitmap_cvg` 会为 0，stability 数字变成空洞的 100% |

方案 A 与「A/B 对比报告」最契合，也最可能产出有意义的 T3 数字；方案 B 是计划原文的字面读法。
