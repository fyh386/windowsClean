<h1 align="center">WindowsClean</h1>

<p align="center">
  基于 Everything SDK 的轻量级 Windows 清理软件（C++ / Win32，性能优先），<br>
  附带同设计的 Linux 版 <b>UbuntuClean</b>（GTK3 / C++17）。
</p>

<p align="center">
  <img alt="语言" src="https://img.shields.io/badge/%E8%AF%AD%E8%A8%80-C%2B%2B%20%C2%B7%20Win32%20%2F%20GTK3-4c8dff">
  <img alt="许可" src="https://img.shields.io/badge/%E8%AE%B8%E5%8F%AF-MIT-3fb950">
  <img alt="依赖" src="https://img.shields.io/badge/%E4%BE%9D%E8%B5%96-Everything%20SDK%20%28MIT%29-8a5cff">
  <img alt="占用" src="https://img.shields.io/badge/%E7%A9%BA%E9%97%B2%20CPU-%E7%BA%A6%200%25-ffc131">
</p>

## 功能（Windows 版）

- **日志清理**：通过 Everything 秒级检索全部日志文件
  - 白名单直收：`log / trace / etl / evtx / log1 / log2`
  - 黑名单排除：exe/dll/图片/媒体/压缩包/Office/源码/配置/数据库（含 ldf）等已知非日志格式
  - 无后缀 / 未知后缀文件：并行文本探针（8 线程，读文件头 64KB），兼容 UTF-8 / UTF-16 / GBK
  - 按名称 / 位置 / 大小 / 修改时间排序；勾选后批量删除到回收站（含管理员重试）
- **大文件查找**：预设 50 / 100 / 500 / 1000 MB 与自定义阈值，按大小排序，可批量删除
- **压缩包管理**：查找 zip / 7z / rar / tar / gz / iso 等压缩包，按大小排序，批量删除到回收站
- **安装包管理**：查找 msi / exe / appx 等安装包，按大小排序，批量删除到回收站
- **久未使用**：基于 Everything 运行历史，找出很久未打开 / 使用的文件（默认 90 天，可选手动文件或程序模式），按最后使用时间排序，批量删除到回收站
- **内存清理**：待机列表清理（RAMMap 风格，需管理员，自提权子进程执行）、工作集压缩、进程查看与结束
- **系统信息**：硬件信息（操作系统 / CPU / 内存 / 显卡 / 主板 / 磁盘）+ 实时 CPU 占用、内存占用、运行时间与 CPU 温度（每秒刷新）
- **软件卸载**：注册表枚举已安装程序（名称 / 版本 / 发布者 / 大小 / 位置），一键卸载、打开目录、官网、系统设置页
- **单文件分发**：Everything.exe 与 everything64.dll 作为资源内嵌，首次运行自释放到 `%LOCALAPPDATA%\WindowsClean\Everything\` 并静默启动，无需单独安装 Everything

## 功能（UbuntuClean · Linux 版）

位于 [`ubuntu-cleaner/`](ubuntu-cleaner/)，GTK3 原生桌面程序，C++17，扫描与删除全部在后台线程执行：

- **日志清理**：白名单后缀直收 + 黑名单排除；无后缀 / 未知后缀并行文本探针（UTF-8 / UTF-16 / GBK）
- **大文件**：按阈值（50 / 100 / 500 / 1000 MB 或自定义）查找
- **久未使用**：按最后修改时间找出超过 N 天未使用的文件；可选“程序”模式
- **压缩包 / 安装包管理**：按后缀枚举，批量删除
- **内存清理**：实时内存、进程列表、结束进程、`pkexec` 授权清理（sync + drop_caches）
- **系统信息**：OS / 内核 / CPU / 内存 / 显卡 / 主板 / 磁盘 + 每秒刷新的占用与温度
- **软件卸载**：枚举 dpkg / snap / flatpak，`pkexec` 授权卸载
- 删除一律先进回收站（GIO `g_file_trash`，遵循 XDG Trash 规范）

## 构建（Windows）

要求：Windows 10/11 x64、Visual Studio 2022（含 C++ 桌面开发）、PowerShell。

```powershell
.\scripts\build.ps1            # Release
.\scripts\build.ps1 -Config Debug
```

构建脚本自动下载 Everything 1.5 便携版与 Everything SDK（MIT 许可）到 `third_party\`，生成图标并编译。产物位于 `bin\x64\Release\WindowsClean.exe`（单文件，约 5~7MB）。

运行测试：

```powershell
.\bin\x64\Release\WindowsClean.Tests.exe
```

## 构建（UbuntuClean，Ubuntu 24.04）

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libgtk-3-dev

cd ubuntu-cleaner
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ubuntuclean          # 运行
./build/ubuntuclean-tests    # 测试
```

## 性能设计

- 查询走 Everything IPC（毫秒级），结果用 `LVS_OWNERDATA` 虚拟列表渲染，20 万行不卡顿
- 引擎连通性用非阻塞探针检测；索引未就绪时显示“正在建立索引”而不是挂起
- 排序用并行 `std::sort`，默认修改时间降序（日志页）/ 大小降序（大文件页）
- 文本探针有预算上限（默认 50,000 个 / 次）与文件大小上限（默认 64MB），可设置
- 静态链接（/MT）、无 MFC，空闲 CPU 占用约 0%
- UbuntuClean 扫描与删除全部在后台线程，UI 不阻塞

## 配置

配置文件：`%LOCALAPPDATA%\WindowsClean\config.ini`（Windows，排除路径、探针上限、结果上限等，可在“设置”页修改）；UbuntuClean 配置文件位于 `~/.config/ubuntuclean/settings.ini`。

## 目录结构

```
src/                Windows 版源码（Win32 单文件应用，无 MFC）
tests/              Windows 版测试（GoogleTest）
scripts/            构建 / 依赖下载 / 图标生成脚本
ubuntu-cleaner/     Linux 版（GTK3 + CMake，独立构建）
third_party/        Everything 便携版与 SDK（构建时自动下载，不入库）
```

## 许可

- 本程序：MIT（见各源码文件头部）
- Everything / Everything SDK：Copyright (C) 2008-2026 voidtools，MIT 许可，允许免费再分发
