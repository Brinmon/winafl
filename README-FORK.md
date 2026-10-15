# WinAFL

本仓库是 [googleprojectzero/winafl](https://github.com/googleprojectzero/winafl) 的 fork，
用于维护一个可在现代 Windows + VS2022 环境下**开箱即编译**的版本，并做定制开发。

上游 master 自 2025-12 起 CI 处于红灯状态（连续 failure / cancel），本 fork 修复了
阻塞编译与运行的若干问题，使 x64 Release 可完整构建并通过端到端冒烟测试。

---

## 与上游的差异（fork patches）

### 1. 修复 XED/mbuild 的 CRT 不匹配（构建阻塞）
`third_party/TinyInst/third_party/CMakeLists.txt` 调用 XED 的 `mfile.py` 时，
mbuild 默认以 `/MT`（静态 CRT）编译 XED，而 TinyInst 及其余目标是 `/MD`（动态 CRT），
导致链接 `afl-fuzz` / `afl-showmap` 时报 `__imp__stricmp` / `__imp_realloc` 无法解析。

- **修复**：给 `mfile.py` 传 `--no-mscrt`（关闭 mbuild 的隐式 CRT 选择），
  再通过 `_CL_` 环境变量统一注入 `/MD`。
- 该补丁在 `build_all.bat` 中**构建时幂等应用**（不改动 submodule 指针）。

### 2. 修复 DynamoRIO 客户端 `atoi` 符号冲突（`winafl.dll` 链接阻塞）
新版 DynamoRIO 的 `ntdll_imports.lib` 与 WinAFL 硬编码链接的静态 `libucrt.lib`
都导出 `atoi`，触发 `LNK2005`。这也是上游 CI 红灯的直接原因之一。

- **修复**：`CMakeLists.txt` 为 `winafl` 目标追加 `/FORCE:MULTIPLE`。

### 3. 修复管道 EOF 断言弹窗（运行期误报）
`winafl.c` 的 `ReadCommandFromPipe()` 不检查 `ReadFile` 返回值。当 fuzzer 进程
退出/被强杀、管道关闭后，该函数返回未初始化的垃圾值，既非 `'F'` 也非 `'Q'`，
导致目标进程弹出 `ASSERT FAILURE: unrecognized command received over pipe` 对话框。

- **修复**：`ReadCommandFromPipe` 检测 `ReadFile` 失败/读到 0 字节时返回 `0`（EOF），
  两处 pipe 命令处理（`pre_loop_start_handler` / `pre_fuzz_handler`）把 `0` 视作
  与 `'Q'` 相同的优雅退出路径（`dr_exit_process(0)`），不再断言弹窗。

### 4. 启用符号定位（`USE_DRSYMS` + PDB）
`-target_method`（按函数名定位目标）需要符号。构建开启 `USE_DRSYMS=1` 并产出 PDB，
否则 Release 构建无法解析非导出函数，会弹 `Can't find specified method in target_module`。

### 5. 中文代码页兼容
通过 `_CL_=/utf-8` 修复源码在中文 Windows（代码页 936）下的 `C4819` 警告被
`/WX` 升级为错误的问题。

---

### 6. 修复便携包 DynamoRIO 运行时缺失（`-D` 模式不可用）
`package.ps1` 原先只拷贝 `lib64\release\*`，遗漏了 `lib64\` 顶层的运行时 DLL
（`drpreinject.dll`、`drconfiglib.dll`、`drinjectlib.dll`、`drearlyhelp*.dll`、
`ntdll_imports.dll`），导致 `-D` 模式直接报：

```
ERROR: cannot find required file ...\dynamorio/lib64\drpreinject.dll
```

线上 `v1.18.0` 的 `winafl-portable-win64.zip` 即受此影响（DR 模式仅能靠 TinyInst 代偿）。

- **修复**：`package.ps1` 改为按位宽拷贝 `bin64`/`bin32` + `lib64`/`lib32` +
  `ext/lib64`、`ext/lib32` 下的全部 **exe/dll**（排除 `.pdb`/`.lib`），并在步骤 [6]
  做完整性强制校验（17 项必需路径 + 源 `lib64`/`lib32` 顶层 DLL 逐一对账），缺一即
  **构建失败**。
- **两个关键事实**（写进脚本注释，避免再犯）：
  1. `-D` 必须指向**含 `drrun.exe` 的目录**（`bin64` / `bin32`），而不是 DR 根目录——
     `afl-fuzz`/`afl-showmap` 拼接的是 `<DrDir>\drrun.exe`；
  2. DynamoRIO 由 `drrun.exe` 位置反推根目录，并要求根目录**完整**：
     `lib64` 与 `lib32` 都要有 `release/` 与 `debug/` 的 `dynamorio.dll` 及 `drpreinject.dll`，
     否则会警告 `does not appear to be a valid DynamoRIO root`（64 位目标缺 32 位库也会警告）。
- **体积**：包体从 ~199MB 降至 **85MB**（改用可执行文件/库精选而非全目录递归，
  过滤掉了 `.pdb`/`.lib`/debug 符号）；zip 约 24MB。
- **新增**：`smoke-test.ps1` 默认跑 **TinyInst + DynamoRIO 双模式**（`-Mode both`），
  CI 增加“打包后从 `dist/` 再跑一次双模式”步骤。此前 CI 只测 TinyInst，因此该缺陷
  得以静默出厂。

## 构建方法

前置条件：
- Visual Studio 2022（含 C++ 工作负载）
- Python 3
- DynamoRIO（放在 `D:\HackerCode\devtools\DynamoRIO`，或修改脚本路径）

一键构建（x64 Release，含 TinyInst + DynamoRIO 双模式）：

```bat
build_all.bat
```

产物在 `build64\bin\Release\`：
`afl-fuzz.exe` `afl-showmap.exe` `afl-tmin.exe` `afl-analyze.exe` `winafl.dll` +
测试 harness（`test.exe` 等）+ PDB。

> 若把 DynamoRIO 放在别处，编辑 `build_all.bat` 里的 `-DDynamoRIO_DIR=...`。
> 已验证可用版本：DynamoRIO cronbuild-11.91.20735（旧版 10.0.19741 因缺
> `drmgr_register_exit_event` 无法编译最新上游代码）。

---

## 端到端冒烟测试

实测（x64，宿主 i9-12900HX / Win11 23H2）：fuzz 循环正常，`execs_done` 持续增长，
强杀 fuzzer 后目标进程优雅退出、无弹窗。

### 模式性能对比（`test.exe`，30s 基准）

| 模式 | 吞吐 (execs/sec) | 稳定性 | 说明 |
|---|---|---|---|
| **TinyInst (`-y`)** | **~24000** | **100%** | 静态编入 `afl-fuzz.exe`，最便携、最快 |
| DynamoRIO (`-D`) | ~5600 | 95% | 需捆绑 DynamoRIO 运行时，最灵活 |
| Intel PT (`-P`) | — | 50% | 硬件追踪，但在 Alder Lake 混合核上报 `err=87` |

> **Intel PT 说明**：理论上最快（硬件分支追踪、~2x 开销），但 i9-12900HX 是
> P 栋(Golden Cove)+E 栋(Gracemont) 异构微架构，线程迁移破坏 trace，IPT 驱动
> 查询 trace 报 `err=87`。功能已编入（`-P` 可用），但在混合核 CPU 上不稳定。
> **实测推荐用 TinyInst 模式（`-y`）。**

## 便携包（整包携带）

`package.ps1` 生成 `dist\winafl-portable\`，可整包拷到**任意 Win10+ x64**：

```
winafl-portable\
├─ afl-fuzz.exe (含 TinyInst + Intel PT)  afl-showmap/tmin/analyze.exe
├─ winafl.dll (DynamoRIO 客户端)  winaflpt-debug.exe  libipt.dll
├─ *.pdb (符号定位/调试)
├─ msvcp140.dll vcruntime140*.dll (VC 运行库，目标机无需装 VC++ Redist)
├─ demo.cmd (TinyInst 演示)  demo-dynamorio.cmd (DynamoRIO 演示)
├─ in\ (演示种子)
└─ dynamorio\ (bin64+bin32+lib64+lib32+ext，供 -D 模式用；仅 exe/dll)
```

- 核心 TinyInst 模式**自包含**，不依赖 DynamoRIO
- DynamoRIO 运行时按位宽完整携带（64 位目标也需要 32 位库，否则 DR 判根目录无效）
- VC 运行库随包分发，干净机器拷贝即用（已验证）
- 总包约 **85MB**（zip 约 24MB）

用法示例（TinyInst，推荐）：
```bat
cd winafl-portable
afl-fuzz.exe -y -i in -o out -t 30000 -- ^
  -instrument_module target.dll -target_module target.exe -target_method fuzz_one ^
  -nargs 2 -iterations 5000 -persist -loop -- target.exe @@
```

用法示例（DynamoRIO；`-D` 指向**含 drrun.exe 的目录**）：
```bat
cd winafl-portable
afl-fuzz.exe -i in -o out -t 30000 -D dynamorio\bin64 -w %~dp0winafl.dll -- ^
  -coverage_module target.dll -target_module target.exe -target_method fuzz_one ^
  -nargs 2 -- target.exe @@
```

## Docker 容器（整合在本仓库）

Dockerfile 直接放仓库根目录，构建上下文指向 `package.ps1` 产出的
`dist\winafl-portable`（单一产物，不复制两份）。一键构建：

```powershell
.\docker-build.ps1                 # 编译 + 打包 + docker build
.\docker-build.ps1 -SkipBuild -SkipPackage   # 复用现有 dist，仅 docker build
```

镜像内 WinAFL 已预编译，无需容器内构建。实测（ltsc2022 + Hyper-V 隔离）：

```powershell
docker run --rm winfuzz:latest cmd /c C:\winafl\demo.cmd   # 跑内置演示
docker run --rm -it winfuzz:latest cmd                       # 进 shell
```

- 容器内跑 TinyInst 模式：`execs_done` 增长、`stability 100%` —— 已验证
- `container-test.ps1` 是挂在容器里跑的有界测试脚本，供回归验证

## CI/CD（GitHub Actions）

`.github/workflows/build.yml`：push/tag 自动构建，产出 `winafl-portable-win64.zip`：

- runner `windows-2022`（自带 VS2022；上游旧的 `windows-2019` 已停服）
- 递归拉子模块 → 下载 DynamoRIO 11.91 → 应用 xed 补丁 → 全模式构建
- 自动跑 TinyInst 冒烟测试（失败则红灯）→ 打包 → 上传 artifact
- 打 `v*` tag 时自动创建 GitHub Release 并附 zip

发布新版本：
```bash
git tag v1.18.0 && git push origin v1.18.0
```

---

## 定制开发方向（TODO）

- [ ] 将 3 个构建补丁整理为可提交给上游的 PR（含 CI 修复）
- [ ] 修复上游 GitHub Actions（`build.yml` 仍在用 `windows-2019` + VS2019）
- [ ] TinyInst 模式（`-y`）端到端验证（当前已编译，冒烟走的 DynamoRIO 模式）
- [ ] Intel PT 模式（`-DINTELPT=1`）构建开关
- [ ] 容器化：把本仓库构建流程接入 `win-fuzz-base` Dockerfile

## 许可

Apache-2.0（继承上游，见 `LICENSE.txt`）。
