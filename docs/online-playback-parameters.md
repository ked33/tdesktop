# 优化在线播放参数核对

面板包含 22 个数值项和 2 个开关，对应 23 个独立配置字段。按修复后的调用链核对：4 个控件仅内置播放器使用，3 个仅 MPV 使用，17 个由两者共用。

“MPV”包括应用内的普通 MPV 播放和特殊播放入口；这些设置不会自动应用到从系统中单独启动的 MPV。下载通道和非会员总并发参数还会影响同一账号、同一媒体服务器上的普通文件下载。

## 配置何时生效

- 保存会写入全部档位，播放只读取当前实际启用档位。参数缓存于进程内，保存后需要重启应用。
- “仅智能档”分组有 8 个数值项。其中 5 个仅非会员使用：恢复上限，以及服务器总并发的 4 个参数，已在各自说明中注明。
- MPV 的两个缓存参数只有在优化档位不是“已禁用”、前向缓存值大于 0 时才覆盖 MPV 选项。0 表示沿用 MPV 配置，不表示关闭全部缓存。
- 数字表示基础值或预算时，实际数量还取决于文件剩余数据、请求优先级、缓冲恢复和服务器等待。并非每个场景都会触发每个参数。

## 全部参数与消费位置

| 参数 | 配置字段 | 适用播放器 | 生效条件与实际作用 | 代码 |
| --- | --- | --- | --- | --- |
| 同时下载块数 | `requestsLimit` | 内置和MPV | 普通读取的基础请求预算；速度自适应、智能档、通道容量和服务器等待共同决定实际并发。 | [Reader] |
| 提前下载块数 | `preloadPartsAhead` | 内置和MPV | 当前读取区间之后的基础预读量，会受自适应和恢复策略调整。 | [Reader] |
| 从文件末尾读取的块数 | `tailPrefetchParts` | 内置 | 内置远程读取启动时提交文件尾预读；由下方开关控制，0 关闭额外预读。 | [Source]、[Reader] |
| 拖多远就停掉旧下载 | `seekCancelJumpParts` | 内置和MPV | 开启旧下载清理后使用的读取位置跳变阈值，单位是块；智能恢复可跳过该次清理。 | [Reader] |
| 拖动后留下附近块数 | `seekCancelGuardParts` | 内置和MPV | 跳变清理时在当前读取区间前后保留的额外块数；智能档还保护音视频所需范围。 | [Reader] |
| 提前准备多少毫秒的画面 | `loadInAdvanceMs` | 内置 | 内置远程播放器按媒体时长暂停、恢复提前读取的目标。 | [Player] |
| 卡顿后先缓冲多少毫秒的画面 | `waitingBufferMs` | 内置 | 播放卡顿后恢复所需的媒体缓冲时长；非会员智能档取它与自适应恢复目标的较大值。 | [Player] |
| 拖动进度条后停止旧位置的下载 | `seekCancelEnabled` | 内置和MPV | 控制拖动相关旧请求清理；必要的音视频、索引和恢复请求仍会保留。 | [Reader] |
| 打开视频时先读文件末尾 | `tailPrefetchParts > 0` | 内置 | 派生开关：关闭保存为 0；开启且数值为 0 时保存为 1，不是另一个独立配置字段。 | [Panel]、[Source] |
| 每条通道一开始允许多少块在途 | `startWaitedParts` | 内置和MPV | 新建下载通道的初始在途容量，按媒体服务器共享，也影响普通文件下载。 | [Manager] |
| 每条通道最多允许多少块在途 | `maxWaitedParts` | 内置和MPV | 单通道在途容量的增长上限；不能小于初始容量。 | [Manager] |
| 一开始用几条下载通道 | `startSessions` | 内置和MPV | 媒体服务器下载通道的初始数量，后续下载复用已有通道。 | [Manager] |
| 最多用几条下载通道 | `maxSessions` | 内置和MPV | 通道数量的增长上限；不能小于初始数量，超时可使通道减少。 | [Manager] |
| 从文件末尾读取的块数 | `mpvTailPrefetchParts` | MPV | 普通和特殊 MPV 入口创建读取器时使用；只对 MP4、MOV、M4V 等匹配文件启用。 | [MPV]、[MPVSpecial]、[Reader] |
| 前向缓存上限（MiB） | `mpvCacheMaxMb` | MPV | 优化档位大于 0 且值大于 0 时，传给 demuxer-max-bytes；0 沿用 MPV 设置。 | [MPV]、[MPVSpecial] |
| 往回保留的缓存（MiB） | `mpvCacheBackMb` | MPV | 与前向缓存一起传给 demuxer-max-back-bytes；前向缓存为 0 时不单独覆盖。 | [MPV]、[MPVSpecial] |
| 被放慢后的基础预读上限 | `nonPremiumPreloadLimit` | 内置和MPV | 仅非会员智能档的服务器等待后恢复阶段；码率和拖动恢复需求可高于该基础上限。 | [Reader] |
| 被放慢时至少提前下载几块 | `smartMinimumPreload` | 内置和MPV | 智能档基础预读下限；索引、首帧和紧急缺失数据可使用更小预算。 | [Reader] |
| 被放慢时保留几块下载请求 | `smartMinimumRequests` | 内置和MPV | 智能档读取请求预算下限；等待服务端解限时只保留待下载请求，不保证继续发送。 | [Reader]、[Manager] |
| 正常时最多提前下载几块 | `smartMaximumPreload` | 内置和MPV | 智能档正常播放的额外预读上限；索引及恢复阶段可使用独立预算。 | [Reader] |
| 刚开始播放时同时下载几块 | `smartInitialRequestLimit` | 内置和MPV | 仅非会员智能档，媒体服务器首次建立智能调度状态时的初始总并发预算。 | [Manager]、[Reader] |
| 同时下载块数下限 | `smartMinimumRequestLimit` | 内置和MPV | 仅非会员智能档，自动总并发预算及限速恢复的下限；服务端等待时仍会停止新增请求。 | [Manager]、[Reader] |
| 同时下载块数上限 | `smartMaximumRequestLimit` | 内置和MPV | 仅非会员智能档，同一服务器总并发预算上限；视频和普通下载共享。 | [Manager]、[Reader] |
| 网速够用时减到这个块数就停止 | `smartCapacityMinimumRequestLimit` | 内置和MPV | 仅非会员智能档，网速够用时降低并发的停止值；服务端放慢仍可进一步降低。 | [Manager] |

## 本次核对修正

1. 内置文件尾预读原先只有未被调用的取值函数，数值和开关没有触发实际预读。现由内置文件读取启动流程提交请求，与 MPV 的独立文件尾参数分开生效。
2. 特殊 MPV 入口原先未上报播放码率，按播放需求调节并发的逻辑无法完整工作。现与普通入口一样上报需求，并在主读取器、拖动读取器和故障重建之间交接，避免把一个视频重复统计为多个播放需求。
3. “被放慢后的提前下载上限”实际仅在非会员智能档的恢复阶段使用，已移入智能分组，并改称“基础预读上限”，说明恢复需要可提高预算。
4. “缺数据时先等多少毫秒”实际是卡顿后恢复播放所需的媒体缓冲时长，已更名并重写说明。起播和拖动后的启动缓冲另有策略。
5. 服务器要求等待期间不会继续发送新请求。“被放慢时至少同时下载几块”已改为“被放慢时保留几块下载请求”，说明请求预算与实际发送的区别。
6. MPV 前向缓存、往回缓存是两个额度，前向缓存不是 MPV 总内存上限；缓存单位统一标为 MiB。MPV 到达文件末尾后，还可能按自身设置把未使用的前向额度用于往回缓存。

## 读取与缓存的边界

单块固定为 128 KiB。文件尾预读预算会按块边界对齐，最后一个不完整块可能使请求数量略多于配置的整块数；不超过 10 MiB 的文件跳过额外文件尾预读。关闭额外预读不会禁止解析器按需读取文件末尾。

“正常时最多提前下载几块”限制读取区间之外的额外预读，不限制已经缓存的数据总量，也不会阻止 MPV 按自身前向缓存目标继续提出正常读取请求。

MPV 选项语义参考：[官方 options.rst](https://github.com/mpv-player/mpv/blob/master/DOCS/man/options.rst) 中的 `demuxer-max-bytes`、`demuxer-max-back-bytes` 和 `demuxer-seekable-cache`。

## 验证范围

核对覆盖 UI 字段映射、数值范围、保存、序列化、读取、调用入口和最终消费分支。本机没有 Telegram 构建依赖和可运行产物；结论属于源码静态核对，未逐项以真实账号和实际播放器实测。

[Panel]: ../Telegram/SourceFiles/boxes/enhanced_options_box.cpp
[Source]: ../Telegram/SourceFiles/media/streaming/media_streaming_source.cpp
[Reader]: ../Telegram/SourceFiles/media/streaming/media_streaming_reader.cpp
[Player]: ../Telegram/SourceFiles/media/streaming/media_streaming_player.cpp
[Manager]: ../Telegram/SourceFiles/storage/download_manager_mtproto.cpp
[MPV]: ../Telegram/SourceFiles/media/streaming/media_streaming_mpv.cpp
[MPVSpecial]: ../Telegram/SourceFiles/media/streaming/media_streaming_mpv_special.cpp
