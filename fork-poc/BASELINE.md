# 基线（persistent 模式，必须被 fork 模式打败）

日期：2026-10-15
目标：QQ NT 9.9.26 `libvips-42.dll`（20MB，图片解码器，x64）
harness：`E:\fuzzwork-real\harnesses\qq_image_vips`（`vips_image_new_from_buffer`）
引擎：WinAFL 1.17 (TinyInst `-y -persist -loop -iterations 2000`)
种子：20 个真实 QQ 图片资源
时长：10 分钟（1 实例，单核）

| 指标 | 值 | 备注 |
|---|---|---|
| execs_done | 23,906 | |
| execs_per_sec | 43.83 | |
| corpus_count | 55 | |
| **stability** | **16.72%** | ⚠️ 数据不可信（状态污染） |
| bitmap_cvg | 12.73% | |
| unique_crashes | 0 | 此 stability 下的 crash 也不可信 |
| unique_hangs | 0 | |

原始文件：`baseline/fuzzer_stats_persistent.txt`、`baseline/runs.txt`、`baseline/persistent.log`
（在 `E:\fuzzwork-real\baseline\`）

## fork 模式的验收标准

| 指标 | 目标 | 理由 |
|---|---|---|
| **stability** | **≥ 95%** | 唯一能证明"状态干净"的客观指标（基线 16.72%） |
| execs_per_sec | ≥ 43.83 的 0.5x | 克隆有开销，但不应慢到失去意义 |
| corpus 增长 | 在同等 execs 下更快 | 噪声减少 → 新路径信号更纯 |
| crash 可复现性 | 100% 可复现 | 干净状态下发现的 crash 必须能重放 |
