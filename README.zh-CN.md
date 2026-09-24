# Ace Combat 5 · 静态重编译版（简体中文说明）

**皇牌空战 5：未被歌颂的战争**（PS2）的静态重编译版本，可在 Windows 上原生运行。

游戏的**主 CPU 代码不是模拟的**。一个 Python 工具会预先读取 PS2 可执行文件，把每个函数翻译成 C 代码；GCC 再把它和一套运行时一起编译。这套运行时顶替了主机的其余部分：GS 图形（走 Vulkan）、VU 向量单元、SPU2 音频、IPU 影像解码、IOP 模块、记忆卡和手柄。最终产物是一个普通的 `ac5.exe`。

画面同样是原生的：3D 部分（飞机、座舱、地形、地面物体、天空与云）不经过模拟的 GS，游戏在 VU1 上跑的向量程序已经被改写成原生代码，几何体以真正的 GPU 网格绘制，带顶点着色器、mipmap 贴图和各项异性过滤。平显、雷达和无线电字幕按你的窗口分辨率绘制，任何尺寸下都清晰。原生渲染器暂未覆盖的部分（菜单、机库、部分特效）仍然走模拟 GS，画进同一帧。

支持两种版本，x64 与 Windows on ARM 都能构建：

| 版本 | 序列号 | 配置文件 |
| --- | --- | --- |
| 美版 | SLUS-20851 | `config/` |
| 日版 / 汉化版 | SLPS-25418 | `config/cnjp/` |

---

## 关于本仓库

本仓库是 [sal063/Ace-Combat-5-Static-recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation) 的派生版本。

> **本项目由 DeepSeek Harness 调用 DeepSeek V4.1 Flash 模型修改。**

在原项目基础上增加了：汉化版（日版基底）支持、Windows on ARM（arm64）构建、以及若干区域无关化与构建体验方面的改进。详见文末「与上游的差异」。

本仓库**不包含任何游戏代码或素材**。你需要自备游戏副本，重编译器在你的机器上从它生成 C 代码。

---

## 一、准备工作

### 1. 必需软件

| 软件 | 说明 |
| --- | --- |
| **64 位 Windows** | 需要支持 Vulkan 的显卡驱动 |
| **[MSYS2](https://www.msys2.org)** | 提供 GCC 和 SDL3。在 MSYS2 UCRT64 终端里执行： |

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-pkgconf
```

GCC **必须 15 或更新**。生成的代码依赖保证尾调用（`[[gnu::musttail]]`），编译器太旧会得到 CMake 警告，并且程序可能耗尽栈。

| 软件 | 说明 |
| --- | --- |
| **CMake** 3.20+ | 构建系统 |
| **Ninja** | 推荐用 `pip install ninja` 安装 |
| **[Vulkan SDK](https://vulkan.lunarg.com)** | 构建时用其中的 `glslc` 编译着色器 |
| **Python 3** | 只用标准库，无需 pip 安装任何东西 |

### 2. 游戏文件

需要以下两个文件之一（放在哪个目录都行，启动时用参数指定）：

- 美版：`SLUS_208.51`，或美版 ISO
- 日版/汉化版：`SLPS_254.18`，或日版/汉化版 ISO

校验哈希，确认版本正确：

```powershell
Get-FileHash .\SLUS_208.51 -Algorithm SHA256
#   C3594227605307806592416DBD723BF5771952E279EE281A40DA305426667385   应为 3,634,092 字节

Get-FileHash .\SLPS_254.18 -Algorithm SHA256
#   B510EE45343325BDACF14B81E3A1B34DE103C1F0379BEAA014795A4E401025AD   应为 3,634,988 字节
```

哈希不匹配就**不要继续**，那说明版本不对或文件被改过，编译结果对不上配置文件。

> **ISO 不要放进仓库目录。** 光盘镜像有 4 GB 以上，远超 GitHub 的单文件 100 MB 上限，而且不是我们能分发的。`.gitignore` 已经忽略 `*.iso`。

### 3. 设置终端

后续命令都是 PowerShell，在仓库根目录执行。先把 MSYS2 的 UCRT64 `bin` 放到 PATH 最前面：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
```

只对当前窗口生效。MSYS2 不在 `C:\msys64` 的话改路径。刚装完 Vulkan SDK 的话，请开一个新终端让它读到 `VULKAN_SDK`。

---

## 二、一键构建（推荐）

```powershell
# 先检查环境缺什么，不做实际构建
pwsh -File tools/build_all.ps1 -Why

# 构建全部三个目标：汉化版 x64、美版 x64、汉化版 arm64
pwsh -File tools/build_all.ps1

# 只构建汉化版 x64
pwsh -File tools/build_all.ps1 -What cnjp

# 只构建美版 x64
pwsh -File tools/build_all.ps1 -What us

# 只构建 arm64 交叉版本
pwsh -File tools/build_all.ps1 -What arm64

# 连重新编译（recompile）也重做
pwsh -File tools/build_all.ps1 -Recompile

# 某个目标失败时继续构建其余目标
pwsh -File tools/build_all.ps1 -KeepGoing
```

脚本会依次完成：检查工具链（缺什么直接告诉你，而不是跑到一半失败）→ 从 ISO 里提取可执行文件（如果你已经提取过就跳过）→ 重新编译 → 配置 CMake → 编译。需要 arm64 时会自动下载交叉工具链。

**常用参数：**

| 参数 | 作用 |
| --- | --- |
| `-What all\|cnjp\|us\|arm64` | 构建哪些目标，默认 `all` |
| `-Recompile` | 强制重做重新编译步骤 |
| `-Why` | 只检查环境，不构建 |
| `-ElfDir <路径>` | 游戏可执行文件所在/提取到的目录 |
| `-UsIso` / `-CnjpIso <路径>` | 指定 ISO 路径 |
| `-Msys2 <路径>` | MSYS2 UCRT64 目录，默认 `C:\msys64\ucrt64` |
| `-BuildType Release\|Debug` | 构建类型 |

**产物**（每个目录里都有 `ac5.exe`、两个 DLL 和 `shaders\`）：

```
build/x64-cnjp/ac5.exe      汉化版 x64
build/x64-us/ac5.exe        美版 x64
build/arm64-cnjp/ac5.exe    汉化版 arm64
```

编译要几分钟——每个生成的文件基本上就是一个巨大的函数，GCC 处理起来很慢。12 线程 CPU 大约 3~5 分钟。

---

## 三、启动

### 方式一：启动脚本（推荐）

仓库提供了 `tools/run.ps1`，会自动找到 ISO、数据目录和 exe：

```powershell
# 汉化版
pwsh -File tools/run.ps1 -Region cnjp -Iso "D:\Games\ac5cnjp.iso"

# 美版
pwsh -File tools/run.ps1 -Region us -Iso "D:\Games\ac5us.iso"
```

常用参数：

```powershell
pwsh -File tools/run.ps1 -Region cnjp -Iso .\ac5cnjp.iso -Log run.log   # 日志存文件
pwsh -File tools/run.ps1 -Region cnjp -Trace                           # 详细日志
pwsh -File tools/run.ps1 -Region cnjp -SkipChecks                      # 跳过启动前检查
```

要把参数直接传给 `ac5.exe`，用 `-ExtraArgs`，**每个参数单独一项**：

```powershell
pwsh -File tools/run.ps1 -Region cnjp -ExtraArgs '--frames','300'      # 跑 300 帧
pwsh -File tools/run.ps1 -Region cnjp -ExtraArgs --seconds 20
```

> 不要写成 `'--frames,300'`：PowerShell 会把它当成**一个**参数传下去，程序认不出来。
>
> 全屏请在游戏里按 **F11** 或 **Alt+Enter** 切换，没有对应的命令行参数。
>
> `--frames` / `--seconds` 到达上限后游戏会停止推进并打印 `run summary`，但**进程不一定会自己退出**（有个后台线程还活着）。看到 summary 后自己关窗口即可。

### 方式二：直接命令行

```powershell
# 汉化版
.\build\x64-cnjp\ac5.exe --data generated-cnjp --disc "D:\Games\ac5cnjp.iso" --watchdog 0

# 美版
.\build\x64-us\ac5.exe --data generated --disc "D:\Games\ac5us.iso" --watchdog 0
```

### 参数说明

| 参数 | 说明 |
| --- | --- |
| `--data` | **重编译输出目录**（含 `ps2_image.bin` 的那个），即 `generated-cnjp` 或 `generated`。**不是光盘目录。** 指错了程序会在进入游戏前停下，并打印它尝试过的每一个路径 |
| `--disc` | ISO 文件，或已解包的光盘目录 |
| `--watchdog 0` | 关闭看门狗。默认情况下，10 秒内没有新画面就退出；调试时有用，正常玩请关掉 |
| `--verbose` | 输出大量日志 |
| `--frames N` / `--seconds N` | 运行 N 帧 / N 秒后退出 |
| `--novideo` | 无视频输出运行 |

**`--data` 必须和 `--disc` 的版本匹配**：`config/` 配 `generated/` 和美版 ISO；`config/cnjp/` 配 `generated-cnjp/` 和 `SLPS_254.18`。混用会得到一个能启动、但永远停在第一屏的构建。

窗口出现后**会黑屏约 20 秒才出第一张画面**，这是正常的，等一下。

---

## 四、操作

手柄通过 SDL 识别，凡是被 SDL 认出的手柄都能直接用。按键按位置映射：下方面键是叉、右是圈、左是方、上是三角；肩键是 L1/R1，扳机是 L2/R2，按下摇杆是 L3/R3，Back 和 Start 是 Select 和 Start。

键盘默认：

| PS2 | 键盘 |
| --- | --- |
| 方向键 | 方向键 |
| 叉 / 圈 / 方 / 三角 | X / S / Z / A |
| L1 / R1 | Q / E |
| L2 / R2 | 1 / 3 |
| 左摇杆 | 小键盘 8 / 4 / 2 / 6（W 也可作上） |
| Start / Select | Enter / 右 Shift |

都可以在设置菜单里改。

其他按键：

- **F4** 打开/关闭设置菜单
- **F11** 或 **Alt+Enter** 切换全屏
- **Esc** 退出，或关闭设置菜单
- **F6~F10** 是调试热键（截图和状态转储），可以忽略

**存档**写在启动目录下的 `saves` 文件夹，是这个运行时自己的格式，不是 PCSX2 的记忆卡格式。用 `PS2_SAVE_DIR` 可以改位置。

---

## 五、目录说明

| 路径 | 内容 |
| --- | --- |
| `tools/ps2recomp/` | 重编译器（输入 ELF，输出 C） |
| `tools/vurecomp/` | VU1 微程序重编译器 |
| `tools/cnjp/` | 把美版配置翻译到日版/汉化版的工具 |
| `tools/build_all.ps1` | **一键构建脚本** |
| `tools/run.ps1` | **一键启动脚本** |
| `tools/export_release.ps1` | **打包成独立文件夹** |
| `tools/launch.ps1` | 独立文件夹内的启动器 |
| `runtime/` | 顶替主机的一切，加上设置菜单和 mod 层 |
| `runtime/src/rn/` | 原生渲染器 |
| `runtime/dll/` | 构建时拷到 `ac5.exe` 旁边的 SDL3 和 pthread（按架构分） |
| `config/` | 美版的 IDA 导出、符号表、override 和 hook |
| `config/cnjp/` | 同一批文件翻译到日版/汉化版的结果 |
| `cmake/toolchain-arm64.cmake` | Windows on ARM 交叉编译工具链 |
| `generated/`、`generated-cnjp/` | 重编译输出（构建时生成，已忽略） |
| `dist/` | 打包输出（已忽略） |

---

## 六、打包成独立文件夹

把构建结果打成一个**可以直接拷走、双击运行**的文件夹：

```powershell
# 先确认你了解授权问题（见下）
pwsh -File tools/export_release.ps1 -Region cnjp -PersonalUseOnly

# 美版 / arm64 / 自定义输出目录
pwsh -File tools/export_release.ps1 -Region us   -PersonalUseOnly
pwsh -File tools/export_release.ps1 -Region cnjp -Arch arm64 -PersonalUseOnly
pwsh -File tools/export_release.ps1 -Region cnjp -Out D:\AC5 -PersonalUseOnly
```

输出默认在 `dist/ac5-<region>-<arch>/`（约 74 MB x64 / 87 MB arm64），内容是：

```
ac5.exe                 游戏本体
SDL3.dll                必需
libwinpthread-1.dll     必需
ps2_image.bin           必需（见下）
shaders\                11 个 .spv，必需
launch.cmd              双击启动
launch.ps1              启动脚本
README.txt              中英双语说明
LICENSE
```

拿到文件夹的人：**把它拷到任意 Windows 机器**，把自备的 ISO 放进去（或启动时指定），**双击 `launch.cmd`** 即可。

`launch.ps1` 会自动在文件夹内、`iso\`、上级目录、`Documents`、`Downloads`、`Desktop` 里找 `*.iso`，也会检查镜像大小（小于 1 GB 会警告），并会拒绝在 x64 上启动 arm64 版本。也可以在文件夹里手动运行：

```powershell
.\launch.ps1                                   # 自动找 ISO
.\launch.ps1 -Iso D:\Games\ac5cnjp.iso         # 指定 ISO
.\launch.ps1 -List                             # 只显示找到了什么，不启动
```

### ⚠️ 关于 `ps2_image.bin`（重要）

这个文件**是必需的**，删掉它程序会在启动阶段卡住，**窗口一直黑屏**。

它同时**是游戏可执行代码的副本**（从你重编译的那个可执行文件里取出，约 3.5 MB）。所以：

- 打包脚本默认**拒绝执行**，必须先加 `-PersonalUseOnly` 确认你明白这一点
- 打出来的文件夹**只供自己使用，不要上传或分发**
- 如果你的目的是公开分享，正确做法是分享源码，让别人在自己机器上重编译 —— 这样 `ps2_image.bin` 会在他们本地生成
- `.gitignore` 已经忽略 `dist/`，防止误提交

---

## 七、与上游的差异

在 [sal063/Ace-Combat-5-Static-recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation) 基础上的改动：

### 1. 汉化版 / 日版支持

日版可执行文件和美版**不是同一个程序**，中间有代码插入和删除，所以美版的配置文件不适用。`tools/cnjp` 用机械化、可复现的方式完成翻译，而不是手工移植：

```powershell
$env:PYTHONPATH = "$PWD\tools"
python -m cnjp map  --source SLUS_208.51 --target SLPS_254.18 --ida-db config/ida_db.json -o addr_map.json
python -m cnjp port --map addr_map.json --config config --out config/cnjp --region cnjp
```

两个可执行文件有大量逐字节相同的代码段，只是地址不同，所以工具会遍历两边的 `.text`，记录每一段的固定偏移。**这些偏移是分段的**——共 26,388 个区间，并不存在一个统一的偏移量（相关区域分别是 −0x28、+0x0、+0x8、+0x2B0、+0x328）。

翻译质量：

| 项目 | 数量 |
| --- | --- |
| 检查的美版函数 | 6,767 |
| 逐字节验证通过 | 6,622（97.9%） |
| 在预测地址附近搜索找回 | 50 |
| 仅按 opcode 匹配找回 | 81 |
| 无法定位 | 14 |
| 美版重编译 | 10,742 个函数，占 `.text` 98.04% |
| 汉化版重编译 | 10,670 个函数，占 `.text` 97.6% |

### 2. 运行时改为区域无关寻址

运行时代码里凡是写死客户机地址的地方，都必须能在另一个版本里找回来。**不存在一个通用偏移**，所以改为启动时扫描已加载的可执行文件来定位：

- `rn_tap.c`（原生渲染器的接入点）为每个条目保存一段指令特征（指令值加逐字掩码），靠匹配定位。
- `rn_intent.c` 还写死了约 40 个函数。它们分属**五个不同的偏移区间**，逐个定位行不通：屏蔽立即数后，它们的 `addiu sp, sp, -N` 序言会匹配到上千个位置。所以按**区域整体**定位——先扫描该区域首个条目，唯一命中就得到该区域偏移，再套用到全区域成员并逐条复核。
- 校验读的是**加载时的可执行文件镜像**，不是客户机内存：游戏会用自己的 overlay 覆盖 `.text` 的一部分，等渲染器初始化时，图元写入器读回来已经是无关代码了。

### 3. 黑屏的真正原因

汉化版曾经能启动、能加载光盘、所有 hook 都挂上、日志**没有任何报错**、垂直同步稳定在 59.9 Hz——**但屏幕一直全黑**。

根因是**一个硬编码地址**。`NUSNDSTR`（声音流模块）有一个计数器，客户机在音色库传输完成时递增它，场景状态机要等它推进才会进入下一屏：

- 美版：计数器在 `0x0047EC9C`，计数 1 → 2
- 日版/汉化版：计数器在 **`0x0047F49C`**（同一变量，偏移 +0x800）

运行时在两个版本里都读美版地址。在汉化版上那个地址恒为 0，于是游戏永远在等一个**其实已经完成**的传输，停在第一屏不动，什么都没画出来。

现在改为**观测决定**：两个已知地址都读，哪个在动就用哪个。

### 4. Windows on ARM（arm64）构建

需要 x86_64 的 clang 驱动加 aarch64 sysroot——注意 `C:\msys64\clangarm64\bin\clang.exe` **本身就是 AArch64 程序**，在 x64 主机上不能当交叉编译器用，这是个坑。`tools/setup_arm64_toolchain.py` 会自动获取两者。

另外 `-fpatchable-function-entry`（mod 钩子用的可修补入口雪橇）在 Windows 上用 clang 时会关闭，因为 clang 的 SEH 写法和它对于序言长度的判断不一致，会导致 AArch64 编译报 `Incorrect size for func_... prologue`。

### 5. 构建体验

- 自动把 SDL3 和 pthread 拷到 `ac5.exe` 旁边（按架构选对），程序在任意目录都能直接双击运行
- `--data` 会自动搜索常见输出目录；失败时打印尝试过的所有绝对路径
- 新增 `tools/build_all.ps1` 一键构建脚本、`tools/run.ps1` 一键启动脚本、`tools/export_release.ps1` 打包脚本
- 新增 `tools/capture.ps1` 截图脚本（见下）
- 缺少 `ps2_image.bin` 时快速报错并说明原因，而不是打开一个一直黑屏的窗口

### 6. 截图脚本 `tools/capture.ps1`

排查画面问题时最麻烦的一件事是**截图**：游戏窗口必须显示出来才能截，而终端窗口
又会挡住它，截出来的经常是终端而不是游戏。

这个脚本改用 `PrintWindow`，让**窗口自己把画面画进位图**，因此**不需要窗口可见**，
也不会被任何东西遮挡：

```powershell
# 游戏开着并停在想记录的画面，然后另开一个终端运行：
pwsh -File tools\capture.ps1 -Out sky-on.png

# 换个设置再截一张，用于对照：
$env:PS2_RN_SKY='0'
pwsh -File tools\run.ps1 -Region cnjp -Iso "...\ac5cnjp.iso"
# 停在同一个画面，然后：
pwsh -File tools\capture.ps1 -Out sky-off.png
Remove-Item Env:PS2_RN_SKY
```

参数：

| 参数 | 说明 |
|---|---|
| `-Out` | 输出路径，默认 `work\shots\cap_<时间戳>.png` |
| `-MaxWidth` | 超过此宽度就缩放，默认 1280，`0` 表示不缩放 |
| `-Screen` | 改用屏幕截图（会先隐藏其他窗口）。**只在 `PrintWindow` 截出黑图时使用**，脚本会告诉你是否还有东西挡在前面 |

**注意：** 这个脚本**只移动窗口、不改动游戏**。如果 `PrintWindow` 在某些驱动上
截出黑图，用 `-Screen`，它会隐藏所有其他窗口、把游戏提到前面、截图、再恢复。

---

## 八、常见问题

**CMake 报 `Cannot find source file: .../generated/ps2_func_table.c`**
还没重新编译，或者 `-o` 输出到了 `generated` 以外的目录。

**CMake 找不到 Vulkan、`sdl3` 或 `glslc`**
第二节的 PATH 那行没在这个窗口执行；或者 MSYS2 包 / Vulkan SDK 没装；或者装 SDK 时终端已经开着。

**CMake 警告编译器没有 musttail 属性**
GCC 太旧，需要 15 或更新。

**`ac5.exe` 起不来，报缺少 DLL**
构建已经自动把 `SDL3.dll` 和 `libwinpthread-1.dll` 拷到 exe 旁边了。如果还是报错，检查这两个文件在不在 `ac5.exe` 同目录。`vulkan-1.dll` 来自显卡驱动，不随仓库分发。

**日志说 `vk: cannot open shader`**
`shaders` 文件夹不在 `ac5.exe` 旁边了。放回去，或者用 `PS2_SHADER_DIR` 指定 `.spv` 文件所在目录。

**程序自己退出，日志有 `WATCHDOG: the guest delivered no field for 10 seconds`**
漏了 `--watchdog 0`。

**窗口一片黑，但日志看着一切正常**（光盘加载了、hook 挂上了、59.9 帧/秒、无报错）
几乎总是 `--data` 指向了**另一个版本**的 `ps2_image.bin`。配置和可执行文件必须是同一个版本。见第三节末尾的说明。

**日志出现 `nusndstr: the transfer counter is at ...`**
在汉化版上这是**正常**消息，表示运行时已经找到了移动过的声音计数器。如果场景不再推进、而日志显示计数器一直停在 0，说明你的可执行文件对应的计数器地址不对。

**推送到 GitHub 报 `File ... exceeds GitHub's file size limit of 100.00 MB`**
ISO 被提交进去了。`.gitignore` 现在会忽略 `*.iso`，如果你已经误提交，先从索引里移除：`git rm --cached *.iso`。

---

## 九、授权

皇牌空战是 Bandai Namco Entertainment 的商标。本项目与它们没有任何关联，也未获其认可。仓库不含任何游戏文件，也不会提供，请不要索取。

代码以 Apache License 2.0 发布，见 `LICENSE`。Dear ImGui 是 MIT 许可，许可证在 `third_party/imgui/LICENSE.txt`。Lua 同样是 MIT，见 `third_party/lua/LICENSE.html`。

英文原版说明见 [`README.md`](README.md)。
