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

---

## 完整验证结果（2026-10-15）

### 能力验证（全部 PASS）

| 验证项 | 数据 |
|---|---|
| `RtlCloneUserProcess` 可用（x64 / Win10 19045） | ntdll 导出 ✅ |
| 共享段穿透 CoW（子写父读） | `cov=[5A 0C]` ✅ |
| 私有内存 CoW 隔离 | 父状态未被污染 ✅ |
| 2000 次克隆 + 真实 libvips 解析 | **0 timeout, 0 leak** ✅ |
| 父进程句柄数 | **135 → 135**（恒定）✅ |
| 克隆体崩溃 → 父进程存活并识别 | **200/200** ✅ |
| 克隆体内 LoadLibrary | 成功 ✅ |
| 吞吐 | **198–356 clones/sec** ✅ |

### 5 个文档记录的坑 — 实测结果

| 坑 | 结果 |
|---|---|
| P1 句柄表空洞（ALPC/独占句柄） | ⚠️ 存在但无害（克隆体句柄 12 vs 父 141） |
| P2 其他线程不克隆 | ❌ 无影响 |
| P3 同步原语死锁 | ❌ 未发生（2000 次 0 timeout） |
| P4 克隆体内不能 LoadLibrary | ❌ 证伪（实测成功） |
| P5 映射视图 ViewUnmap | ❌ 证伪（共享段穿透） |

### 工具清单

| 文件 | 用途 |
|---|---|
| `clone_poc.c` | 基础克隆 + CoW 隔离验证 |
| `shared_mem_poc.c` | 共享段穿透 CoW 验证 |
| `pitfall_test.c` | 5 个坑 + 泄漏 + 吞吐综合测试 |
| `crash_test.c` | 克隆体崩溃隔离 + 分类 |
| `stability_test.c` / `cov_stability.c` | 稳定性指纹对比（方法待改进） |
| `DEVELOPMENT-PLAN.md` | ★ 详细开发计划 |
