# 实战案例：libvips-42.dll 的 stability 归因（16.72% → 98.72%）

> 完整版：`E:\GitCode\winafl\attribution\ATTRIBUTION.md`（L1–L8 逐层证据 + 干预表）

## 根因（函数级铁证）

| 机制 | 占比 | 证据 |
|---|---|---|
| **libvips GObject `class_init` 块（每进程一次）** 被特定输入路径首次触发时正落在校准/发现运行内 → 污染 `first_trace` | 6870/6995 = **98%** | omida 反编译 `sub_180024300`=`tiffsave_base.class_init`、`sub_1800099A0`=`dzsave_base.class_init`；6221 字节签名 old=1→new=0 全在 run=1 |
| operation cache 容量边界(100)穿越校准窗口 + 默认 24 线程计数漂移 | ~110 字节 | 探针实测 cache 每次+1、第100次撞满；`conc_init=24` |
| 长跑回塔：新输入（magic匹配但坏chunk）的首现路径落在发现运行，**AFL 用发现时 trace 残留播种 first_trace**（`afl-fuzz.c:3641→3646`，上游设计；AFL++ PR#425 在相邻路径打过同类补丁） | id60=6245 字节 | run2/run3 逐字节相同的确定性复现 |

## 干预链（每步只动一个变量）

| 干预 | stability | var_bytes |
|---|---|---|
| 基线复现 | 16.49% | 6995 |
| + fuzzer 目标入口清 trace（对齐 DR `winafl.c:506` 语义） | 16.65% | 6949（-46 = warmup 贡献） |
| + harness 预热（有效输入） | **93.87%** | 105 |
| + 关缓存 + 单线程（OSS-Fuzz 同款） | **99.01%** | 17 |
| 10 分钟长跑暴露回塔（坏PNG首现） | 19.17% | 6267 |
| + 垃圾预热（无效——路径类型不对） | 19.17% | 6267 |
| + **坏 PNG 预热样本** | **98.72%**（10 分钟 24790 execs） | 22 |

## 判据口径修正

- `bitmap_cvg` **12.73% 是虚高**（含一次性 init 死重量），修复后诚实口径 = **2.6%**。新目标一律用修复后口径对比。
- 达标线：stability ≥90%（黄线 95%）+ variable_paths 个位数 + eps ≥ 基线 0.5×；不必追 100%。

## 分诊要点

1. 玩具目标 100% 不可外推（玩具 warmup 恰好烧完一次性路径）。
2. 签名速查：**old=1→new=0 且集中 run=1** = 进程首现/lazy init；**run=0 且 old>0** = 发现运行残留播种；
   **计数 ±且分散** = 线程/时序。
3. 上游 WinAFL 的 `mark_as_variable` 落盘被砍（`.state/variable_behavior/` 恒空），案发名单要靠诊断补丁或自行归因。
4. 预热修不好、首现路径无穷多 → 升级 fork 路线（`E:\GitCode\winafl\fork-server`）或非 persistent。
