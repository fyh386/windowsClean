# UbuntuClean（Linux 版 WindowsClean）

基于 GTK3 的 Ubuntu 24.04 系统清理工具，C++17 编写，延续 Windows 版“性能优先、
占用小”的设计：界面为原生 GTK 桌面程序，扫描与删除全部在后台线程执行。

## 功能

- **日志清理**：扫描全盘/用户目录，白名单后缀（log/trace/etl/evtx/log1/log2）直收，
  黑名单（可执行、图片、媒体、压缩包、文档、源码、数据库等）排除；无后缀/未知后缀
  文件并行读头做文本探针（UTF-8/UTF-16/GBK 兼容，可只保留含换行的文本）。
- **大文件**：按阈值（50/100/500/1000 MB 或自定义）查找所有格式的大文件。
- **久未使用**：按最后修改时间找出超过 N 天未使用的文件；可选“程序”模式
  （可执行位或常见程序后缀）。
- **压缩包管理 / 安装包管理**：按后缀枚举压缩包与安装包，可批量删除到回收站。
- **内存清理**：实时内存占用、进程列表（名称/PID/内存）、结束进程、
  `pkexec` 授权清理内存缓存（sync + drop_caches）。
- **系统信息**：操作系统/内核/主机名/CPU/内存/显卡/主板/磁盘 + 每秒刷新的
  CPU 占用、内存占用、运行时间与 ACPI 温度。
- **软件卸载**：枚举 dpkg / snap / flatpak 已安装软件，`pkexec` 授权卸载。
- **设置**：探针开关、结果上限、排除路径（前缀匹配）。

删除一律先尝试进回收站（GIO `g_file_trash`，遵循 XDG Trash 规范）；回收站不支持的
分区会在失败后询问是否永久删除。

## 构建（Ubuntu 24.04）

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libgtk-3-dev

cd ubuntu-cleaner
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/ubuntu-cleaner
```

运行时只依赖系统自带的 GTK3（Ubuntu 24.04 桌面默认包含），无需安装其他组件。

## 目录结构

```
src/
  main.cpp     入口
  app.cpp      GTK 界面（9 个页签）
  scanner.cpp  后台多线程文件扫描 + 分类过滤
  probe.cpp    扩展名白/黑名单与文本探针
  filelist.cpp 排序
  trash.cpp    XDG 回收站删除
  sysinfo.cpp  系统硬件/实时信息/温度
  memclean.cpp 内存信息、进程、drop_caches
  uninstall.cpp dpkg/snap/flatpak 枚举与卸载
  util.cpp     通用工具
```

## 已知限制（v1）

- “久未使用”按修改时间判断；Linux 默认 relatime 挂载下访问时间不可靠。
- 全盘扫描跳过 /proc /sys /dev /run /snap 等虚拟或重复文件系统；
  回收站目录自动排除。
- 结果上限默认 20 万条，可在“设置”中调整；超大结果集下列表插入需要约 1 秒。
