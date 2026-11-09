# Windows 私有提交内存分析

管理员权限仅用于运行采集脚本；Telegram 必须保持普通权限。此工具用于定位占用来源，不执行内存清理，也不修改客户端配置或账号数据。

## 准备匹配的 EXE 与符号

从同一次 `Build Windows x64` Actions 运行下载：

- `Telegram-x64`：`Telegram.exe`、`build-info.json`。
- `Telegram-x64-symbols`：`Telegram.pdb`、相同的 `build-info.json`。

正常退出客户端后更新 EXE，符号解压到单独目录，例如 `D:\D-Download\Telegram-x64-symbols`。不要用不同运行或旧版本的 PDB。脚本会对两份文件的大小和 SHA256 做严格校验；启用符号前发布的旧 EXE 无法补用新构建的 PDB。

云构建保留 Release 优化，使用 `/Z7` 与链接器 PDB。首次启用符号会改变编译缓存键，构建时间、对象文件和符号产物会变大。符号产物保留 14 天；本地应保存与所用 EXE 配套的一份。第三方库若没有调试信息，栈可能只能解析到模块边界。

## 预检查与采集

需要 Windows x64、64 位 PowerShell 7、系统自带的 WPR。查看结果需安装 Windows ADK 中的 Windows Performance Toolkit（WPA）；不需要本地 Telegram 构建环境。

在仓库根目录，可先在普通 PowerShell 7 只读校验文件：

```powershell
& '.\Telegram\build\diagnostics\capture_memory.ps1' `
  -ExePath 'D:\D-Software\64Gram Desktop\Telegram.exe' `
  -SymbolsDirectory 'D:\D-Download\Telegram-x64-symbols' `
  -CheckOnly
```

然后从托盘菜单正常退出所有 `Telegram.exe`。打开**管理员 PowerShell 7**，在仓库根目录运行：

```powershell
& '.\Telegram\build\diagnostics\capture_memory.ps1' `
  -ExePath 'D:\D-Software\64Gram Desktop\Telegram.exe' `
  -SymbolsDirectory 'D:\D-Download\Telegram-x64-symbols' `
  -OutputDirectory 'D:\D-Download\Telegram-memory' `
  -MaxSeconds 600
```

按终端提示操作，每步完成后回终端按 Enter：

1. 从普通桌面或资源管理器启动客户端，等聊天列表稳定。不要在管理员终端中启动客户端；脚本会检查目标进程的提升状态。
2. 浏览能够复现内存增长的聊天、图片；停止播放，关闭媒体查看器，回终端按 Enter，建立清理前标记。
3. 点击增强设置的“清理”，等完成后回终端按 Enter。不要切换聊天。脚本再等 3 秒记录清理后标记，保存 ETL，并恢复跟踪设置。

不要在采集中更新 EXE、重启客户端或启动其他同名实例。脚本校验路径、PID、启动时间和文件配对，不会替你停止或启动客户端。默认总时限 10 分钟，可设置 60–900 秒；等待操作期间每 250 ms 检查截止时间和磁盘空间，剩余空间低于 2 GiB 时中止。WPR 启停、文件哈希及 ETL 合并是同步操作，期间没有硬超时保护，合并可能超过采集时限。WPR filemode 的文件大小没有硬上限。

采集会临时修改 64 位 IFEO `Telegram.exe` 的 `TracingFlags` 堆跟踪位，并恢复原值。已有堆跟踪或 IFEO 路径过滤时拒绝启动。Heap 跟踪按映像名启用，VirtualAllocation 记录包含系统范围事件，因此 ETL 可能包含其他进程的名称、路径和地址；文件只保存在指定本地目录，不自动上传。

## 中止与异常恢复

按 Q 正常中止；常规异常和 Ctrl+C 通过 `finally` 尝试恢复原配置并取消本脚本的 WPR 实例。中止时不保留完整 ETL。不要直接关闭窗口或杀死采集器，否则无法保证 `finally` 执行。

每次采集开始前会显示独立目录和 `recovery.json`。如果窗口被强制关闭，在管理员 PowerShell 7 中运行：

```powershell
& '.\Telegram\build\diagnostics\capture_memory.ps1' `
  -Recover 'D:\D-Download\Telegram-memory\capture-实际目录\recovery.json'
```

恢复只操作固定的 `Telegram.exe\TracingFlags` 和该记录中的独立 WPR 实例，不执行全局 `wpr -cancel`。采集和恢复通过互斥锁避免同时操作设置。若跟踪值被其他工具改动，会拒绝覆盖并报告冲突；不要删除整个 IFEO 键。原先不存在的键可能保留为空键，不含跟踪设置。WPR 的“无运行中的配置”错误 `0xc5583000` 按已取消处理，其他取消错误保留恢复记录并报告；可用 `wpr -status -instancename <实例名>` 核实，不要取消别人的会话。

## 结果分析与修复依据

成功输出 `memory.etl`、`stages.json`、`build-info.json`、`wpr.log` 和 `recovery.json`。阶段 JSON 记录 Private Bytes、Working Set、PID、启动时间；WPR marker 为 `TdMemory:startup_stable`、`TdMemory:before_cleanup`、`TdMemory:after_cleanup`。

在 WPA 打开 ETL，将符号路径指向匹配的 PDB 目录，可加微软符号服务器；加载符号后：

1. 检查事件丢失统计，按 `stages.json` 的 PID/进程启动时间过滤 Heap Allocations 与 VirtualAlloc Commit LifeTimes。确认存在启动阶段的堆事件；丢失分配/释放事件会影响存活大小计算。
2. 对齐三个 marker，按分配调用栈聚合大小与数量，比较清理后仍存活的分配，并区分启动即有的固定占用和浏览后增长。
3. 若活跃分配仍大，定位拥有者和引用释放链；若可重建缓存持续增长，增加预算或淘汰策略；若 Heap Allocations 已明显下降而提交不降，再用 VMMap/堆分析核对空闲页与碎片。VirtualAlloc 可能包含堆后备空间，不能与 Heap Allocations 直接相加。

Private Bytes 不能用工作集替代，VMMap Heap 分类本身也不能区分活对象与空闲空间。不要通过 `EmptyWorkingSet`、反复强制堆整理或强删仍被引用的模型对象制造清理效果。符号与采集工具落地不代表内存问题已经修复，下一次修改应由分配调用栈决定。

## 无管理员权限的脚本检查

```powershell
& '.\Telegram\build\diagnostics\test_capture_memory.ps1'
```

检查覆盖错误 EXE/PDB 配对、恢复记录验证、跟踪设置冲突、失败重试和原值保留；使用临时文件与模拟采集/注册表，不启动 WPR 跟踪、不写系统设置。实际管理员采集需单独验证。

参考：[WPR Heap Analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/recording-for-heap-analysis)、[WPR 命令行](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)。
