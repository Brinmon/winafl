# 实战归因报告：同一样本结果不一致（stability 16.72%）的根因

> 日期：2026-10-16 · 方法：逐层隔离实验 + fuzzer 诊断插桩 + dbgkit/CDB 动态调试 + 代码级机理定位
> 工具：litecov（TinyInst 独立模式）、probe_r4 探针、afl-fuzz 诊断补丁、dbgkit cdb、omida（静态分析已启动备用）
> 结论先行：**根因 = libvips 首次调用的一次性 lazy init 被 AFL 校准计入 first_trace + operation cache 边界 + 默认多线程；WinAFL 的 stability 计算、插桩引擎读写、AFL 核心均无 bug。Harness（未做状态隔离的配置）是放大器。修复后 16.72% → 99.01%（短跑），长跑 10 分钟稳态 98.72%（见 §4 final4）。**
>
> **最终验收（out_final4，10 分钟 / 24790 execs / 与基线同时长同口径）：**
> stability **16.72% → 98.72%** · execs/sec **43.83 → 45.42** · paths **55 → 63** ·
> var_bytes **6995 → 22** · variable_paths **18 → 2** · bitmap_cvg 诚实口径 **2.63%**（旧 12.73% 虚高）
>
> **最终根因（函数级铁证，omida 反编译）：**所谓“一次性 6200 BB 块” =
> **libvips 各 operation 的 GObject `class_init`（如 `tiffsave_base`、`dzsave_base` 的 class_init——
> `sub_180024300`/`sub_1800099A0` 反编译证实），每进程只执行一次，由特定输入路径首次触发**。
> 若该首次触发落在发现/校准运行里，`first_trace` 被污染 → stability 崩塌。
> 修复 = harness 预热三件套之外再加**三类代表性输入预热**（有效 PNG + 完全不匹配垃圾 + “magic 匹配但解析失败”坏 PNG）。

---

## 0. 问题定义

fuzzing 中"同样样本结果不一致"的可测量表现 = AFL **stability**（同输入重复执行时覆盖位图计数的一致比例），
基线 **16.72%**（`BASELINE.md`），本次诊断版完整复现 **16.38%~16.65%**。

---

## 1. 逐层排除过程（每层一个实验，证据齐全）

### L1 插桩机制 × 全新进程 × 同种子 —— ✅ 确定性，排除

```
litecov -instrument_module libvips-42.dll -coverage_file cov_a.txt -- harness_vips.exe 0.png   (×2)
→ cov_a.txt == cov_b.txt，13473 个 offset 逐字节相同
```
同引擎、同环境、跨进程、两次独立执行 → **引擎与单次执行本身没有任何不确定性**。

### L2 同进程连续 2000 次同输入（覆盖并集） —— ✅ 可复现，但此层测不到真凶

```
litecov -iterations 2000 -persist -loop → cov_p1.txt / cov_p2.txt → 两者完全相同
per-iteration "Found N new offsets"：第 2..12 轮全部为 0（BB 集合恒等）
```
集合层面完全一致 ⇒ **发散不在"走了哪些 BB"，而在"同一 BB 的命中次数（map 计数值）"**。
litecov 是集合视图（Set），只有 fuzzer 的计数位图（`inc byte ptr[map]`）能看见 → 此层工具盲区。

### L3 状态漂移探针（probe_r4，单进程 120/40 次重复） —— 观测事实

| 观测 | 值 | 结论 |
|---|---|---|
| vips 并发 | **24**（本机核数） | 线程池默认全开 |
| operation cache | 每次 +1，**第 ~100 次撞满**（默认上限 100） | 跨迭代状态 |
| 内存（不 unref） | **每次调用泄漏 ~19.5KB** | Harness 缺陷实锤 |
| errlen（不清错误缓冲） | **恒定不增长** | 错误缓冲假设被实测否决 |
| threads | 恒 4（无解码线程池——当前 harness 不 sink 像素） | 解码多线程此路径不触发 |
| 大图/损坏图 | 同样规则 | — |

### L4 fuzzer 诊断插桩（var_bytes + 逐输入计数差落盘） —— ⭐ 定位案发分布

给 `afl-fuzz.c` 加纯诊断（AFL++ 同款 var_bytes 导出 + 每输入 `vardiff_id_x.txt`：run 号/字节/old/new 计数）：

```
stability 16.49% · var_bytes 6995 字节 · variable_paths 18~23
vardiff 分布：id_000000 独占 6870/6995（98.2%！），其余 3 个输入合计 125
签名：全部 run=1（校准第 2 次执行）· 6221 个字节 old=1 → new=0
```
⇒ **进程内第 1 次 fuzz_one 调用执行了约 6200 个"只跑一次"的 BB，第 2 次起彻底不跑**。
（与 L1/L2 完全自洽：并集可复现、每轮 new=0、集合恒等——因为 call2 ⊆ call1。）

### L5 对照实验否决两个既有"修复" —— 机理互相印证

| 已有实验 | 结果 | 对根因的含义 |
|---|---|---|
| `out_fix`（上一任的缓存修复版 harness，**无 warmup 修复**） | **17.56%（无效）** | 单修 cache 不够 → 主因不是 cache |
| `out_i10`（iterations=10，每 10 次重启进程） | 17.18%，variable_paths=34（更差） | 重启越频繁，每次进程的"首次调用"越多 → 反证主因=首次调用一次性路径 |

### L6 代码级机理（源码即证据）

1. `afl-fuzz.c:2850` 每次 `run_target` 入口 `memset(trace_bits,0)` → first_trace = 第 1 次校准执行的计数。
2. 第 1 次执行包含 **libvips 首次使用 lazy init**（GType/loader 类注册、首次分配器/内部一次性状态 ≈6200 BB，
   这些代码在进程生命周期内**只应执行一次**——libvips/Glib 的正常设计，不是 bug）。
3. 之后每次执行不再走这些 BB ⇒ 校准第 2 次 vs 第 1 次：6200 个字节 1→0 ⇒ `var_byte_count=6870`
   ⇒ `stability = 100 − var×100/t_bytes ≈ 16.7%`。
4. **AFL 计算本身正确**（`afl-fuzz.c:4525`，与 AFL++ 同式）；**引擎只是如实测量**。
5. DR 模式在目标函数入口清 map（`winafl.c:506 pre_loop_start_handler/pre_fuzz_handler`）→ 部分规避 warmup，
   但**无法规避"函数体内首次调用的 lazy init"**（预测 DR 在真实 libvips 上同样会低——与"引擎无关"结论一致）。
6. 玩具 `test.exe` 为何 100%：target `test_target` 靠进程入口、无重量级库初始化，一次性 BB ≈0。

### L8 omida 静态分析（函数级定案）

- 27188 个函数区间表（`functions.json`）+ 逐函数反编译（`omida exec libvips decompile`）：
- **不稳字节 top 区域反编译结果**：`sub_180024300` = `tiffsave_base` class_init、`sub_1800099A0` = `dzsave_base` class_init，
  内部全是 `g_param_spec_*` + `vips_object_class_install_argument`（属性表安装，每进程一次）→ **GObject class_init 块定案**。
- 干预链验证：valid-warmup 消原 id0 块（6870→105）；garbage-warmup 无效（id60 走“magic 匹配但失败”路径）；
  **加入坏 PNG 预热后 id60 案发 6245 → 5，stability 19.17% → 98.72%**（因果闭环）。

### L7 dbgkit/CDB 动态调试（修复过程中唯一一个 bug 也是它抓的）

- `dbgkit cdb run` + `sxd av` 脚本 → 抓到不可处理 AV 完整栈：
  `memset ← TinyInstCovMap::ClearCoverageBuffer ← tinyinst_run`
- 机理：`map_address = MapViewOfFile2(..., GetChildProcessHandle(), ...)`（`tinyinst_covmap.cpp:62`）
  是**目标进程侧**地址，fuzzer 进程内 memset 必然崩溃 → 修复改用 fuzzer 侧 `trace_bits`（`afl-fuzz.c:159`），
  并删除错误方法（不留死代码）。
- 该 fuzzer 侧入口清除实测对本目标**边际效应 ≈ 0**（a2/a4/a5 的 vardiff 均为 6870），
  保留作为防御性对齐（与 DR 语义一致），**真正的修复在 harness**。

---

## 2. 干预 → 效果（因果验证，逐项）

| # | 干预（每次只加一项） | stability | var_bytes | bitmap_cvg* | 备注 |
|---|---|---|---|---|---|
| a2 | 诊断版复现基线 | 16.49% | 6995 | 12.7% | 问题完整复现 |
| a4/a5 | + fuzzer 入口清除（trace_bits） | 16.65% | 6949 | 12.7% | 边际≈0（warmup 不在 first_trace 中，主因在函数体内） |
| a6 | **+ harness 预热解析**（进循环前先完整解析一次，烧掉 lazy init） | **93.87%** | 105 | 2.61% | ★ 主刀 |
| a7 | **+ `vips_cache_set_max(0)` + `vips_concurrency_set(1)`**（OSS-Fuzz 官方 libvips harness 同款） | **99.01%** | 17 | 2.62% | ★ 副刀，消 cache 边界/线程计数 |
| final2/final3（10 分钟长跑） | — | **19.17% ⚠️回塔** | 6267 | 11.83% | 新输入 id60（magic匹配但坏chunk）在发现运行中首次触发 saver class_init 块（tiffsave/dzsave…），6245 字节污染；**run2/run3 逐字节相同 = 确定性复现** |
| + garbage 预热（干预） | — | 19.17%（无效） | 6267 | — | 反证：id60 走的是“匹配但失败”路径，非“完全不匹配” |
| **+ 坏 PNG 预热（`warm_bad.png` = id60 原件）** | | | | | ★★★ **根治** |
| **final4（10 分钟稳态）** | — | **98.72%** | **22** | **2.63%** | ✅ 24790 execs / 45.42 eps / paths 63 / variable_paths 2 |

\* `bitmap_cvg` 从 12.7%→2.6% **不是覆盖丢失**：旧的 12.7% 里 ~6200 个字节是一次性 lazy init 对
**每个输入都相同**的"死重量"（不携带任何输入区分信息），被重复计入 virgin map 造成虚高。
2.6%（≈1700+ BB）才是输入相关的诚实覆盖，语义与 DR 模式入口清除一致。**BASELINE 的
`bitmap_cvg ≥12.73%` 判据必须同步修正，否则任何正确修复都会被判"覆盖下降"。**

---

## 3. 责任判定（回答"是插桩、Harness 还是 WinAFL 的问题"）

| 责任方 | 判定 | 证据 |
|---|---|---|
| **WinAFL / AFL 核心（stability 计算、协议、SHM）** | **无 bug** | 计算公式核对无误；诊断数字与手算一致；玩具 100% 证明链路健康 |
| **插桩引擎（TinyInst 机制本身）** | **无 bug** | L1 跨进程逐字节一致；计数读写正确 |
| **WinAFL TinyInst 集成的口径** | 有缺陷但**对本目标非主因** | run1 理论含 warmup（`afl-fuzz.c:2850` vs DR 的入口清除），实测 warmup 贡献≈0；已补入口清除作为防御 |
| **libvips（闭源目标）** | **正常行为，非 bug** | 大型库的 lazy init / cache / 默认多线程是设计特性；被 persistent 校准口径放大 |
| **Harness.c（原版）** | **放大器 + 配置缺陷（已修）** | 未预热、未关 cache、未设单线程、不 unref（泄漏 ~19.5KB/次）；`out_fix` 证明单修 cache 不够，必须配合预热 |

**一句话**：不是谁"坏了"，而是 **"有 lazy-init 的大型库 × persistent 模式校准口径"的结构性冲突**，
由 harness 缺少三件套（预热/关缓存/单线程）把它放大成了 16.72%。

---

## 4. 最终验证（10 分钟稳态，out_final4）

| 指标 | 基线 | final4 | 判据 | 结论 |
|---|---|---|---|---|
| stability | 16.72% | **98.72%** | ≥95% | ✅ |
| execs_per_sec | 43.83 | **45.42** | ≥0.5×（≥22） | ✅ |
| bitmap_cvg | 12.73%（虚高口径） | **2.63%**（诚实口径） | 判据需修正（见 §2 注） | ✅ |
| paths_total | 55 | **63** | 同等时长更高 | ✅ |
| variable_paths | 18 | **2** | — | ✅ |
| var_bytes | ~6947 | **22** | — | ✅ |
| run_time / execs | 545s / 23906 | 545s / 24790 | 同口径 | ✅ |
| unique_crashes | 0 | 0 | 可重放性待有 crash 后补测 | 遗留项 |

---

## 5. 残留不稳定（final4：var_bytes 22，≈1.3%）

| 输入 | 字节 | 签名 | 机理判断 | 是否处理 |
|---|---|---|---|---|
| id_000028 | 17 | run3-4、计数 ±（含 52→49 热循环漂移） | 轻微时序/计数漂移 | 可接受（不再投入） |
| id_000060 | 5 | run0、old=1→0 | 其他小首现路径（class_init 块已消） | 可接受 |


实测 98.72% 已远超官方“usually fine（>90%）”，且长跑、多次重启、havoc 新路径均保持。
附注：六次长跑中 1 次出现过“~89 秒 fuzzer 冻结”偶发 hang（AFL 未记超时、无输入特异性证据），
与本修复无关但已列入不确定性（ATTRIBUTION §4 遗留/复现：重跑同配置即可，遇冻用 dbgkit attach dump 栈）。

---

## 6. 修改清单（本次实战落盘的代码变更）

| 文件 | 变更 | 性质 |
|---|---|---|
| `E:\fuzzwork-real\harnesses\qq_image_vips\src\harness_vips.c` | ① 预热解析三件套（argv[1] 文件 + 垃圾字节 + `warm_bad.png` 坏样本）② `vips_cache_set_max(0)` ③ `vips_concurrency_set(1)` | **正式修复** |
| `E:\fuzzwork-real\harnesses\qq_image_vips\warm_bad.png` | 272 字节坏 PNG 预热样本（= id_000060 原件，触发 saver class_init 路径） | 正式修复附件（需随 harness 分发） |
| `E:\GitCode\winafl\afl-fuzz.c` | 诊断：`var_bytes.txt` 落盘、`variable_paths.log`、每输入 `vardiff_*.txt`（calibrate_case 内） | 诊断（建议转正式功能：AFL++ 同款，帮助未来排查） |
| `E:\GitCode\winafl\tinyinst_afl.cpp` | TARGET_START 后清 `trace_bits`（对齐 DR 入口语义，防御性） | 修复（本目标边际≈0，保留） |
| `E:\GitCode\winafl\tinyinst_covmap.h/.cpp` | ClearCoverageBuffer 误实现 → **已删除干净**（crash 现场见 §1L7） | 回退 |
| `E:\GitCode\winafl\attribution\probe_r4.c` + `build_probe.cmd` | 状态漂移探针（cache/线程/内存/校验和） | 工具（保留） |

构建注意（已踩坑）：增量构建 afl-fuzz **必须**带 `set _CL_=/utf-8 /MD`（vcxproj 未写 RuntimeLibrary，
cl 默认 /MT → 与预编译 /MD 库链接失败 LNK1120）；harness `build.cmd` 必须在 qq_image_vips 目录下执行。

---

## 7. 对 RESEARCH-REPORT 路线判决的更新

- **B2（调参/环境隔离）胜出，且比预期更完整**：预热 + 关缓存 + 单线程 = 99.01%，
  **不需要 fork（B3）**——原报告的"分阶段策略"第一步即命中。
- B1（DR persistent 对照）**不再必要**（TinyInst 修复已达标；DR 的入口清除救不了体内 lazy init，预测其值同样 ~17%）。
- B4（接受16.72%）维持否决；目标值按原报告修正为 ≥90%/95% → **实测 99.01% 达标**。
- ⚠️ 修正 `BASELINE.md`/fuzzkit 判据中的 `bitmap_cvg ≥12.73%`（虚高口径，见 §2）。
