# fork-poc — P0 可行性验证：RtlCloneUserProcess

## 目的

验证"Windows 原生进程克隆"能否作为 WinAFL fork-server 的基础，用来替代
persistent 模式（闭源目标下 stability 极低，数据不可信）。

## 结论（2026-10-15，Win10 19045 / x64）

**PASS** — 三项全部成立：

| 验证点 | 结果 |
|---|---|
| `RtlCloneUserProcess` 在 ntdll 中导出（Win10 1809+） | ✅ |
| 克隆体从同一执行点继续、继承内存状态 | ✅ |
| **克隆体的写入不影响父进程**（copy-on-write 隔离） | ✅ |

实测输出：

```
OK: RtlCloneUserProcess found at 00007FFFC77D1E30
  [clone ] pid=37732  counter=1  buffer=BEFORE-CLONE
  [clone ] after tampering: counter=999 buffer=CLONE-TRASHED-IT
  [parent] counter=1  buffer=BEFORE-CLONE
PASS: clone isolation WORKS (parent state untouched by the clone)
```

## 为什么选它而不是其他方案

| 方案 | 速度 | 位宽 | 换目标成本 | 换机器成本 |
|---|---|---|---|---|
| **RtlCloneUserProcess** | 中（内核 CoW） | **32+64** | **零**（通用 API） | 零 |
| target-embedded (winfuzz) | 最快 7–182x | 仅 32 位 | **高**（每目标定制 hook） | 零 |
| WINNIE CSRSS fork | 中 | 32+64 | 零 | **极高**（偏移按系统逆） |

目标多且持续增加 → 通用机制优先。

## 已知限制（实现时必须处理）

- 不复制内核对象：文件/网络句柄为共享语义，harness 内需要时自行重开
- 只克隆调用线程（对我们有利：单线程 fuzz）
- 非公开 API：参考实现 `huntandhackett/process-cloning`、`rbmm/RtlClone`

## 构建与运行

```bat
build.cmd        :: 需要 VS2022 (vcvars64)
clone_poc.exe
```
