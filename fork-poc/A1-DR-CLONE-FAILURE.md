# A1 决定性实验：DR 插桩下能否 fork —— 结论 FAIL

> 日期：2026-10-16 · 环境：Win10 19045 / x64 / VS2022 · DR 随 WinAFL portable 分发
> 目的：验证方案 A（fork 引擎运行在 DynamoRIO 插桩之下）的**唯一前提**——
> DR 插桩后的进程能否安全 `RtlCloneUserProcess`，且克隆体继续执行插桩代码。

## 结论

**FAIL。DR 的插桩状态无法穿过 `RtlCloneUserProcess`。**

| 实验 | 克隆体退出码 | 说明 |
|---|---|---|
| 无 DR（基线对照） | `0x00000000` ×3 | 克隆正常，插桩目标函数执行、日志、正常退出 |
| DR + 普通克隆 | *未产出* | DR 拒绝跟进，应用被终止 |
| DR + `CREATE_SUSPENDED` 克隆 | `0xC0000005` ×3 | DR 接受跟进，但克隆体立即 AV 崩溃 |

## 证据链

### 1. DR 的两种反应（都失败，但方式不同）

**普通克隆**（仅 `INHERIT_HANDLES`，即现 fork-server 的用法）：

```
<Application ...\dr_clone_probe.exe (26912). Failed to follow into child process:
 Child thread not created suspended.>
```

DR 的 follower 期望子进程主线程以挂起状态创建（`follow_children` 机制，
见 `dynamorio.dll` 中的 `follow_children` / `Failed to follow into child process: %s`）。
`RtlCloneUserProcess` 默认不满足，DR 直接终止应用（exit 40）。

**`CREATE_SUSPENDED` 克隆**（`0x1 | 0x2`）：

DR 不再报 "Failed to follow" —— 说明 DR **接受了**这次克隆。但：

```
[33748] iter 0: returned 0x00000000 (proc=0x100 thread=0xFC)   <- 父：克隆成功
[33748] PARENT: resumed clone thread (prev suspend count = 1)  <- 父：恢复克隆线程
[33748] PARENT: clone exited 0xC0000005                        <- 克隆体 AV 崩溃
```

克隆体**从未产生任何自己的日志行**（`probe2.log` 中无独立 PID 行、无 `CLONE:` 行），
即它在进入用户代码分支之前就崩了 —— 崩点在 DR 的进程交接代码里，不在目标代码里。

### 2. 最小化探针排除"探针自身缺陷"

为排除"是否是我探针里 stdio/插桩函数调用导致的"，写了只做 `ExitProcess(0)` 的克隆体：

```c
__declspec(dllexport) __declspec(noinline) int tgt(int s){ ... }   // 插桩目标
...
st = g(0x3, NULL, NULL, NULL, &ci);          // CREATE_SUSPENDED | INHERIT_HANDLES
if (st == ST_CLONED) { ExitProcess(0); }     // 克隆体：除 exit 外什么都不做
```

| 场景 | 结果 |
|---|---|
| 无 DR | `iter 0/1/2 exit=0x00000000`，`parent done` ✅ |
| 有 DR | `iter 0/1/2 exit=0xC0000005` ❌ |

**克隆体除了 `ExitProcess(0)` 什么都不做，在 DR 下依然必崩。** 排除了探针复杂度因素。

### 3. 关键：这是"弹窗阻塞"而非"死锁"

早期实验显示"挂起"，实为 DR/winafl 弹出的**模态对话框**在等人工点击：

> **WinAFL Notice** — Application ... Failed to follow into child process:
> Child thread not created suspended. [确定]

这解释了所有"超时"现象：进程并非死锁，而是在等点击。用 `-msgbox_mask 0` 把
提示改走 stdout 后，真实退出码才暴露（40 = DR 跟进失败终止应用）。

## 为什么失败（机制层）

DR 的插桩是**跨进程**的：DR 库注入目标进程，follower 需要在新进程（或克隆体）中
重新建立 DR 的运行时状态（代码缓存、TLS、内部锁、信号/异常接管）。这条路依赖：

1. 子进程主线程**可被挂起接管**（`CREATE_SUSPENDED` 语义）——`RtlCloneUserProcess`
   默认不提供，需显式加标志；
2. 即使加上标志（DR 接受了交接），克隆体的地址空间是父进程 CoW 视角的**快照**，
   其中 DR 的运行时数据结构（含指向父进程堆、句柄表、线程对象的指针）**并未被一致地重建**。
   克隆只复制调用线程，DR 的后台线程不随之克隆 → 运行时处于自相矛盾状态 → AV。

换句话说：**fork 的"只克隆一个线程 + CoW 快照"与 DR 的"跨进程运行时 + 后台线程"在原理上互斥**，
不是配置问题。

## 对方案选择的影响

- **方案 A 否决**：没有任何配置能让 DR 插桩状态穿过克隆。相关选项
  （`-msgbox_mask 0` 抑制弹窗、`-no_follow_children` / `follow_children` 控制跟进）
  只能改变**失败的表现形式**（弹窗 vs 退出码），不能改变失败本身。
- **A0 的意外收获仍然有效**：DR 插桩下 `test.exe` 的 **stability = 98.31%**
  （`out_a0/fuzzer_stats`，execs 106383 / eps 1760 / paths 12 / 唯一 crash 1）。
  这证明"依赖插桩引擎"这个方向本身是对的，且 DR persistent 已远优于 TinyInst 16.72% 基线。
- **下一步候选**：方案 C（给 x64 补静态插桩，让覆盖率随 harness 编译进去，
  从而与进程所有权解耦）。这是唯一能同时满足"真 fork 干净状态"与"有效覆盖率"的路径。

## 复现命令

```bat
:: 基线（无 DR）
dr_clone_probe2.exe 3 2000 normal      -> RESULT: PASS
dr_clone_probe2.exe 3 2000 suspend     -> RESULT: PASS

:: 有 DR：普通克隆被拒
dynamorio\bin64\drrun.exe -msgbox_mask 0 -c winafl.dll -debug -no_loop ^
  -coverage_module dr_clone_probe2.exe -target_module dr_clone_probe2.exe ^
  -target_method probe_target -- dr_clone_probe2.exe 3 2000 normal
  -> Failed to follow into child process: Child thread not created suspended.

:: 有 DR：CREATE_SUSPENDED 克隆被接受但克隆体崩
  ... same ... -- dr_clone_probe2.exe 3 2000 suspend
  -> clone exited 0xC0000005 (x3)

:: 最小化复现
mini_probe.exe            (无 DR)  -> iter exit=0x00000000 x3
mini_probe.exe + drrun    (有 DR)  -> iter exit=0xC0000005 x3
```

## 产物

| 文件 | 用途 |
|---|---|
| `dr_clone_probe2.c/.exe` | A1 主探针（normal/suspend 对照 + 全程 PID 日志） |
| `dr_clone_probe.c/.exe` | A1 首版探针（发现 DR 拒绝跟进） |
| `../dist/winafl-portable/mini_probe.c/.exe` | 最小化复现（克隆体仅 `ExitProcess(0)`） |
| `../dist/winafl-portable/probe2.log` | 父/克隆 PID 全程日志 |
| `../dist/winafl-portable/out_a0/fuzzer_stats` | A0 证据：DR persistent stability 98.31% |
