# RtlCloneUserProcess fork 引擎 — 详细开发计划

> 目标：把 WinAFL 的 persistent 模式（stability 16.72%）替换为**真 fork 引擎**，
> 期望 **stability ≈ 100%** 且**吞吐不下降**。
>
> 仓库：`E:\GitCode\winafl`（Brinmon fork）
> 前置验证：见 `fork-poc/`（P0/P1 全部 PASS）

---

## 0. 实测结论（决策依据）

### 已验证的能力（`fork-poc/`）

| 验证项 | 结果 | 数据 |
|---|---|---|
| `RtlCloneUserProcess` 可用（x64 / Win10 19045） | ✅ | ntdll 导出 |
| 克隆体写共享段 → 父进程可见（穿透 CoW） | ✅ | `cov=[5A 0C]` |
| 私有内存被 CoW 隔离（干净状态） | ✅ | 父状态不被污染 |
| 重复克隆无死锁（真实 libvips 解析） | ✅ | **2000 次 / 0 timeout** |
| 重复克隆无句柄泄漏 | ✅ | 父句柄 **135 → 135** |
| 克隆体崩溃 → 父进程存活并识别 | ✅ | **200/200 识别** |
| 克隆体内 `LoadLibrary` | ✅ | 竟然成功（但设计上仍不依赖它） |
| 吞吐（克隆 + 真实 libvips 解析） | ✅ | **198–356 clones/sec** |

### 5 个"文档记录的坑"实测

| 坑 | 结论 |
|---|---|
| P1 句柄表空洞（ALPC/独占句柄不复制） | ⚠️ 存在但无害（克隆体句柄 12 vs 父 141），不依赖句柄索引 |
| P2 其他线程不克隆 | ❌ 无影响（句柄恒定） |
| P3 同步原语死锁 | ❌ 未发生（2000 次 0 timeout） |
| P4 克隆体内不能 LoadLibrary | ❌ 证伪（实测成功） |
| P5 映射视图 ViewUnmap | ❌ 证伪（共享段穿透 CoW） |

### 权威数据（USENIX Security '23, "No Linux, No Problem"）

| 对比 | Winnie(forkserver/clone) | WinAFL(persistent) |
|---|---|---|
| 相对 WinFuzz 吞吐 | **7.0x 慢** | **182.7x 慢** |
| stability | — | **快速下降**（nconvert/flac 尤甚） |

论文对 persistent 的定性：*"does not reset process state… overwhelming fuzzers with
false-positive crashes caused by polluted heap and global state"* —— 与我们实测的
16.72% 完全一致。

**结论：RtlCloneUserProcess 路线成立，且换目标零成本（对比 WinFuzz 的每目标定制）。**

---

## 1. 架构

```
afl-fuzz.exe（WinAFL fork）                  harness.exe（我们编译，静态链接 fork 库）
──────────────────────────────                ──────────────────────────────────────
                                                main():
                                                  ├ ① LoadLibrary 闭源 DLL + 解析入口
                                                  ├ ② 打开共享段（cov + ctl）
                                                  ├ ③ 连接 named pipe
                                                  ├ ④ warmup（跑一次，让库进稳态）
                                                  ├ ⑤ 打快照点标记（"此处之后无锁"）
                                                  └ ⑥ fork 循环：
                                                       read pipe 'P'
                                                       ├ RtlCloneUserProcess
                                                       ├ 子进程: fuzz_one() → exit(code)
                                                       └ 父进程: 等子进程 → 写 'K'/'C'+code

run_target()                                   ┌ 共享段（CoW 穿透）
  └ -fork 模式 → forkserver_run()  ────────────┤  cov_map: 65536B（= AFL MAP_SIZE）
       ├ 写 'P' 到 pipe                        │  ctl_map: 4096B（命令/状态/异常码）
       ├ 等 'K'/'C'/'timeout'                  └  均为 CreateFileMapping 命名段
       └ 读 trace_bits（共享段，天然一致）
```

### 关键设计决策

| # | 决策 | 理由 |
|---|---|---|
| D1 | **复用 AFL 现有共享段**（`afl_shm_<fuzzer_id>`），不另造 | `afl-fuzz.c:1502` 已创建，`trace_bits` 已映射；TinyInst 也用它 |
| D2 | **复用现有 named pipe 协议**（`\\.\pipe\afl_pipe_<id>`，命令 `'P'/'F'/'K'/'C'`） | `afl-fuzz.c` 已有完整收发实现，改动最小 |
| D3 | **harness 自己编译**，fork 逻辑静态链接 | 无需 DLL 注入（比 WINNIE/WinFuzz 简单） |
| D4 | **快照点在 warmup 之后** | 确保 libvips 已初始化、无锁持有 |
| D5 | **父进程每 N 次重启**（N 可配，默认 100k） | 防父进程长期累积（TinyInst 已有先例 `cur_iteration == num_iterations`） |
| D6 | **不加 `NO_SYNCHRONIZE`** | 默认会排空线程池 + 获取 ntdll 锁 = 安全克隆 |
| D7 | **覆盖率走共享段，不走 pipe** | 已验证穿透 CoW；pipe 只传控制/状态 |

---

## 2. 交付物

```
winafl/
├─ fork-server/                          # ★ 新增：fork 引擎核心
│   ├─ include/
│   │   ├─ fork_proto.h                  # 共享段 + pipe 协议定义（复用 WINNIE 语义）
│   │   └─ fork_harness.h                # harness 侧 API（宏，类似 EXPOSE_HARNESS）
│   ├─ src/
│   │   ├─ shm.c/.h                      # 共享段创建/打开（cov + ctl）
│   │   ├─ pipe.c/.h                     # named pipe 客户端
│   │   ├─ clone.c/.h                    # RtlCloneUserProcess 封装 + 父子分支
│   │   ├─ crash.c/.h                    # 异常捕获 + 退出码分类
│   │   └─ forkloop.c/.h                 # 主循环（read→clone→wait→reply）
│   └─ forklib.vcxproj                   # 静态库
├─ fork-server/harness-template/          # ★ 新增：harness 模板
│   ├─ harness.c                         # 用户只改 fuzz_one()
│   └─ build.cmd
├─ afl-fuzz.c                            # 改动：init_forkserver + -fork 模式
├─ CMakeLists.txt                        # 改动：FORK=1 编译开关
└─ smoke-test.ps1                        # 改动：加 fork 模式测试
```

---

## 3. 阶段计划

### P2：fork 引擎核心（2 天）

| 任务 | 产出 | 验收 |
|---|---|---|
| **P2.1** 协议定义 | `fork_proto.h` | 结构体与 WINNIE 兼容；有版本号+magic |
| **P2.2** 共享段 | `shm.c` | 双进程读写一致（单测） |
| **P2.3** pipe 客户端 | `pipe.c` | 与 afl-fuzz 握手成功 |
| **P2.4** 克隆循环 | `clone.c` + `forkloop.c` | toy 目标 1000 次无泄漏/无死锁 |
| **P2.5** 崩溃分类 | `crash.c` | `int3`/AV/栈溢出 三类都能识别 |
| **P2.6** 父进程重启 | `forkloop.c` | N 次后重启，计数正确 |

**验收命令**（独立于 afl-fuzz）：
```bat
fork_test.exe --iterations 10000 --target harness.exe
→ 期望: 0 timeout, 0 leak, exit codes 分类正确
```

### P3：WinAFL 集成（1.5 天）

| 任务 | 产出 | 验收 |
|---|---|---|
| **P3.1** 实现 `init_forkserver()` | `afl-fuzz.c` | harness 被启动 + pipe 连接（替代空实现） |
| **P3.2** 新增 `-fork` 模式 | `afl-fuzz.c` + `CMakeLists.txt` | `--help` 显示；与 `-y`/`-D`/`-P` 并列 |
| **P3.3** `forkserver_run()` 接入 `run_target()` | `afl-fuzz.c:2839` 分支 | 三种结局正确分类 |
| **P3.4** 覆盖率走共享段 | `afl-fuzz.c` | `trace_bits` 有值；`fuzzer_stats` 正确 |
| **P3.5** 超时/崩溃路径对齐 | `afl-fuzz.c` | `FAULT_TMOUT`/`FAULT_CRASH` 正确 |

**验收命令**：
```bat
afl-fuzz.exe -fork -i in -o out -t 5000 -- -harness harness.exe -- harness.exe @@
→ fuzzer_stats 有 execs_done/stability
```

### P4：稳定性与性能（1 天）

| 任务 | 验收 |
|---|---|
| **P4.1** 消除克隆热点（句柄继承范围最小化） | 吞吐 ≥ 150/s |
| **P4.2** 父进程重启策略调优 | 30 分钟长跑无退化 |
| **P4.3** 崩溃样本落盘 + minidump | `crashes/` 有内容 |
| **P4.4** 多实例（`-M/-S`）验证 | 并行不冲突 |

### P5：fuzzkit 接入（0.5 天）

| 任务 | 验收 |
|---|---|
| **P5.1** 适配器加 `mode = "fork"` | `fuzz.toml` 可配置 |
| **P5.2** 命令生成（`-fork` + `-harness`） | `command.txt` 正确 |
| **P5.3** preflight 检查（harness 是否含 fork 支持） | 缺支持时报错 |
| **P5.4** 文档（ADR + README-FORK） | — |

### P6：真实目标验证（0.5 天）

| 任务 | 验收 |
|---|---|
| **P6.1** libvips 10 分钟 fork 模式 | 对比基线表 |
| **P6.2** 对比报告 | stability / 吞吐 / corpus |

---

## 4. 测试计划

### 4.1 分层测试矩阵

| 层 | 目标 | 目的 | 通过标准 |
|---|---|---|---|
| **T0** | `fork-poc` 已有 PoC | 回归 | 全部 PASS |
| **T1** | WinAFL `test.exe`（toy） | 协议正确性 | stability ≥ 95% |
| **T2** | `harness_crypt32.c`（纯函数） | **不引入退化** | stability **100%**（基线也是 100%） |
| **T3** | **`harness_vips.c` + libvips** | **核心目标** | stability **≥ 95%**（基线 16.72%） |
| **T4** | 长跑 30 分钟 | 无泄漏/退化 | 吞吐不衰减，句柄恒定 |

### 4.2 验收矩阵（T3 核心）

| 指标 | 基线（persistent） | fork 目标 | 硬性 |
|---|---|---|---|
| **stability** | 16.72% | **≥ 95%** | ✅ 必须 |
| execs/sec | 43.83 | **≥ 40**（不显著下降） | ✅ 必须 |
| corpus 增长 | 55 / 10min | 更快（噪声减少） | 期望 |
| crash 可复现 | 不可信 | **100% 可重放** | ✅ 必须 |
| 内存/句柄 | — | 30 分钟恒定 | ✅ 必须 |

### 4.3 专项测试（针对已实测的坑）

| 测试 | 方法 | 通过标准 |
|---|---|---|
| 死锁 | 10000 次克隆 + 真实解析 | 0 timeout |
| 句柄泄漏 | 每次克隆后 `GetProcessHandleCount` | 恒定 |
| 内存泄漏 | `GetProcessMemoryInfo` 工作集 | 无单调增长 |
| 克隆体崩溃隔离 | 注入 AV/`int3`/栈溢出 | 父进程存活 + 分类正确 |
| 共享段一致性 | 子写父读校验 | 100% 一致 |
| 覆盖率正确性 | 同一输入 N 次，比较元组集 | 一致（stability 100%） |

### 4.4 回归防护

- `smoke-test.ps1` 增加 `-Mode fork`（与 tinyinst/dynamorio 并列）
- CI 三模式全跑
- `fork-poc/` PoC 作为快速回归（< 1 分钟）

---

## 5. 风险登记（全部已实测或已有对策）

| 风险 | 状态 | 对策 |
|---|---|---|
| 克隆不可用 | ✅ 已排除 | — |
| 共享段被 CoW 隔离 | ✅ 已排除 | — |
| 死锁 | ✅ 已排除（2000 次） | 若出现：父进程重启 + 单线程化目标 |
| 句柄泄漏 | ✅ 已排除 | — |
| 克隆体崩溃不可识别 | ✅ 已排除 | — |
| **吞吐下降** | ⚠️ 待 T3 实测 | 若慢：减少继承句柄 + 父进程批量克隆 |
| **父进程长期退化** | ⚠️ 待 T4 实测 | 每 N 次重启父进程 |
| TinyInst 集成 | 🔵 不在本期范围 | fork 模式**不依赖 TinyInst**（覆盖率自建） |
| `RtlCloneUserProcess` 未来变化 | 🟡 低 | 启动时探测，不可用则降级 persistent |
| 32 位目标 | 🟡 未测 | 结构体兼容 x86；单独测试 |

---

## 6. 里程碑

| 里程碑 | 内容 | 预计 | 判据 |
|---|---|---|---|
| **M1** | fork 引擎独立跑通（P2） | Day 2 | 10000 次无泄漏 |
| **M2** | 与 afl-fuzz 集成（P3） | Day 3.5 | toy 目标跑通 |
| **M3** | **crypt32 不退化**（T2） | Day 4.5 | stability 100% |
| **M4** | **libvips stability ≥95%**（T3） | Day 5.5 | **核心目标** |
| **M5** | fuzzkit 接入 + 报告（P5/P6） | Day 6 | 对比表完成 |

---

## 7. 协议定义（`fork_proto.h` 草案）

```c
#define FORK_PROTO_MAGIC   0x4B524F46  /* 'FORK' */
#define FORK_PROTO_VERSION 1

/* 共享控制块（CoW 穿透） */
typedef struct {
    uint32_t magic, version;
    volatile uint32_t cmd;        /* 0=idle 1=run 2=quit */
    volatile uint32_t status;     /* 0=ok 1=crash 2=timeout 3=parse_fail */
    volatile uint32_t exit_code;  /* 子进程退出码 */
    volatile uint32_t exception;  /* 异常码（如 0xC0000005） */
    volatile uint32_t iteration;  /* 迭代计数（父进程用） */
    volatile uint32_t flags;      /* 位标志：crash/timeout/hang */
} ForkCtl;

/* pipe 命令（与 afl-fuzz 现有 'P'/'F'/'K'/'C' 对齐） */
#define CMD_PREPARE  'P'   /* afl-fuzz → harness：准备输入 */
#define CMD_FUZZING  'F'   /* harness → afl-fuzz：开始跑 */
#define CMD_DONE     'K'   /* harness → afl-fuzz：正常完成 */
#define CMD_CRASH    'C'   /* harness → afl-fuzz：崩溃（后跟 DWORD 异常码） */
#define CMD_QUIT     'Q'   /* afl-fuzz → harness：退出 */

/* harness 侧接口（用户只实现 fuzz_one） */
typedef struct {
    const char *dll_path;         /* 闭源目标 DLL */
    const char *input_file;       /* afl-fuzz 写的输入文件（默认 .cur_input） */
    void (*fuzz_one)(const char *input_file);   /* ★ 用户实现 */
    void (*setup)(void);          /* 可选：warmup */
    uint32_t restart_after;       /* 父进程重启阈值（默认 100000） */
} ForkHarness;
```

---

## 8. 与 fuzzkit 的接口

```toml
# harnesses/<name>/fuzz.toml
[instrumentation]
mode = 'fork'                    # ★ 新增
modules = ['libvips-42.dll']     # 信息性（覆盖率由 fork 引擎自建）
target_module = 'harness_vips.exe'
# method/offset 不再需要（fork 引擎在快照点后直接调 fuzz_one）
```

fuzzkit 生成命令：
```bat
afl-fuzz.exe -fork -i <seeds> -o <engine> -t 20000 -I 15000 ^
  -- -harness harness_vips.exe -restart-after 100000 ^
  -- harness_vips.exe @@
```

---

## 9. 开工顺序

1. **P2.1–P2.3**：协议 + 共享段 + pipe（地基）
2. **P2.4**：克隆循环（核心）
3. **P2.6 + P3.1–P3.3**：接入 afl-fuzz
4. **T1 → T2**：先 toy 再 crypt32（确认不退化）
5. **T3**：libvips（核心验收）
6. **P4 → P5 → P6**：调优 + 接入 + 报告
