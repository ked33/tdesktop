# 在线播放诊断日志

在「增强设置 → 优化在线播放」下开启「将在线播放调试日志写入 log.txt」，然后重新打开视频。日志写入客户端工作目录的 `log.txt`，新增记录仍使用 `Video Playback:` 前缀。原有 MPV 调试开关也会启用这些统计。

这批改动只采集诊断信息，保留现有并发、缓冲、取消、降画质和 MPV 兼容策略。日志用于建立可比较的基线，不能据此声称播放性能已改善。

## 关联与采集范围

- 原生播放的 `play_id` 标识一个 Player 对象，`capture` 区分它的播放/采集阶段；`request` 区分启动和每次拖动。原有 `Player seek summary` 追加相同的 `play_id`，其 `gen` 对应新记录的 `seek_gen`。
- `Start document stream` 将文档 `doc`、文件大小、倍速与 `play_id` 关联。切换清晰度可能换成另一份文档，应结合原有降画质日志单独分析。
- `reader_id` 标识 Reader；`reader_capture` 区分其连续采集阶段。下载统计在这个范围内累计，可能跨越同一 Reader 的多次播放。比较快照时计算差值，不要把累计值直接相加。
- MPV 的 `bridge_open` 将文档与桥接 `play_id` 关联；普通和特殊通道分别为 `backend=mpv`、`backend=mpv-special`。隔离 Reader 的 `bridge_play_id` 关联同一次外部播放。`seek_gen` 是隔离读取的代次，`smart_gen` 是普通 MPV 的 Smart 调度代次，两者不可混用。
- 中途开关日志会切换采集代次；后续事件重建的统计标为 `partial=1` 或 `reader_partial=1`。跨过开关切换的 MPV 请求不作为完整请求样本。完整对照请先开启日志，再打开视频。

耗时来自单调时钟，单位为毫秒。不要用 `log.txt` 行首的秒级时间戳相减来衡量拖动延迟。

## 原生播放

| 记录/字段 | 含义 |
| --- | --- |
| `presentation_request` | 启动或拖动请求；包含目标时间、请求序号和倍速。 |
| `presentation` | 请求结果，包含 `elapsed_ms`、`ready_ms`、`user_pause_ms`、`active_ms`。`active_ms` 扣除了用户暂停时间。 |
| `outcome=ui-shown` | 目标帧进入显示流程，并收到 UI 的 `markFrameShown()` 确认。不是显示器扫描输出时间。 |
| `outcome=render` | 不要求 UI 确认的播放路径已调度目标帧显示。与 `ui-shown` 分组比较。 |
| `outcome=audio-progress` | 无视频轨时观察到音频播放位置更新，不是视频首帧。 |
| `outcome=superseded/stopped/error/finished` | 请求在观察到显示结果前结束；不混入成功拖动延迟分位数，另外统计中止率。 |
| `playback_stall_begin` / `stall_end` | 首次呈现之后，由播放器缺数据等待状态造成的停顿。启动/拖动等待和用户暂停分别统计。 |
| `stalls` / `stall_ms` / `max_stall_ms` | 本次采集阶段的缺数据停顿次数、累计时长和最长时长。不是所有可能的解码或渲染掉帧。 |
| `audio_buffer_ms` / `video_buffer_ms` | 各轨已接收数据领先播放位置的媒体时长；未知或已结束的轨道为 `-1`。 |
| `buffer_wall_ms` | 有效轨道缓冲较小值除以当前倍速，估计能覆盖的实际播放时间。 |
| `pending_ms` | 当前启动/拖动请求尚未呈现的持续时间。 |

原生播放器在正常播放期间约每 5 秒输出快照；启动、拖动或缺数据停顿期间约每 2 秒输出。控制变化和请求结果单独记录，停止、播放完毕或出错时输出汇总。已有 ready、关键分片和缓存日志继续保留。

## 下载、缓存和请求

| 字段 | 口径 |
| --- | --- |
| `remote_payload_bytes` | MTProto Loader 实际交付的媒体负载字节数。排除附加下载器直接复用的缓存数据；不含协议开销、未交付的取消响应或 CDN 校验失败数据。 |
| `remote_unique_bytes` | 同一 Reader 采集阶段收到的负载所覆盖的文件区间并集。 |
| `duplicate_payload_bytes` | 收到的负载中重复覆盖同一文件区间的字节。不同 Reader 之间不去重。 |
| `supplied_bytes` / `supplied_unique_bytes` | 成功交给解复用器或 MPV 桥接读取方的字节数/文件区间并集。包含头部探测、重复读取及桥接后台读取，不等于用户实际观看的数据。 |
| `remote_unread_bytes` | 已收到、但在本采集阶段从未成功供应给读取方的文件区间字节数。退出时可作为预读未使用量参考；这些数据可能留在缓存，之后仍有价值。 |
| `downloader_reused_bytes` | 从附加下载器直接复用的字节，不再次算作本 Loader 的网络负载。 |
| `cache_loaded_bytes` | Reader 从缓存载入的分片字节总量，包括重复载入，不是缓存命中率。 |
| `seek_retained_bytes` | seek 取消时保留下来的已派发请求，之后成功交付的字节；仍可能被新位置或后续读取使用。 |
| `queued` / `sent` | 已知排队和已派发、尚未完成的媒体分片数。内部重试/CDN 校验阶段仍属于同一逻辑分片。 |
| `oldest_queue_ms` / `oldest_sent_ms` | 当前最老分片的排队/派发后等待时长。 |
| `max_queue_ms` / `max_request_ms` | 已完成排队/收取阶段中观察到的最长时长。 |
| `cancelled_queued` / `cancelled_sent` | 未派发取消/派发后取消的请求数，包括 Loader 释放时的剩余请求。不能乘以分片大小当作节省的带宽。 |
| `cache_wait_ms` / `remote_wait_ms` | Reader 返回等待状态之后的累计等待时间，包括当前未结束的等待。与播放器实际卡顿不是同一口径。 |
| `preload_parts` / `request_limit` | 最近一次 Reader 计算的预读目标和请求调度上限；不等于 DC 当前实际并发。 |
| `pressure_requested/local/forwarded` | 调用方最后要求的压力状态、Reader 保留的状态、向 Loader 转发的状态。 |
| `pressure_age_ms` | 本地压力连续保留的时间；配合三种压力状态识别过时的压力。 |
| `limited_remaining_ms` / `recovery_remaining_ms` | 最近收到的服务端限制/恢复状态在快照时的剩余时间。 |

文件区间集合和请求关联表各限制在 4096 项。区间跟踪溢出时，依赖精确去重的字段输出 `-1`；请求关联不完整时输出 `requests_complete=0`，依赖完整请求表的字段同样输出 `-1`。原始负载字节计数继续记录。`untracked_completions` 表示无法关联到本次采集排队记录的完成事件。

“重复接收”“未读预取”“保留的旧请求”分别回答不同问题，不能相加当作精确的无效下载总量。

## MPV 桥接

`bridge_summary` 在首个响应块完成及之后有写出进展时尝试输出，普通请求按同一桥接会话约 5 秒合并；读取或锁等待较慢的完成记录允许约 2 秒的间隔。`window_*` 汇总上次输出之后的请求完成数、成功写出量、断连后后台读取量和首块写出耗时最大值。`completed=0` 表示当前请求仍在传输。

- `first_chunk_ms`：从有效 Range 请求进入处理，到第一块响应体成功写完的时间，包含布局探测和读取准备。这不是网络实测首字节，也不是 MPV 显示首帧的时间。
- `mutex_ms`：主读取循环等待 Reader 互斥锁的累计时间。
- `fill_ms` / `max_fill_ms`：锁内读取阶段的累计/最长耗时，包括读前检查与 Smart 预取准备；不包括独立的布局探测和断连后的后台扫描。
- `written_bytes`：成功写完的响应块大小之和。写失败块中可能已经写出的部分不计入。
- `background_read_bytes`：客户端断连后，现有兼容逻辑仍完成的后台扫描读取量，不等于新增网络下载量。
- `requested_offset/length` 保留原始请求范围。兼容模式可能实际从文件开头顺序响应。
- `http_*` 字段是主 Reader 采集阶段的桥接累计计数，分别记录完成数、成功写出量、后台读取量、失败数、断连数和被新请求取代的数量，可在最终 `transfer` 汇总查看被合并的请求。

MPV 没有新增 IPC 状态观察。HTTP Range 也可能来自探测，不能直接当作一次用户拖动。完全阻塞且尚未返回的读取没有桥接定时快照，要等读取返回才能得到完整耗时；原生播放器的等待快照不受这一限制。

## 同带宽对照

1. 固定视频文档、清晰度、倍速、代理节点及带宽限制。冷缓存和热缓存分开；更换节点或自动降画质后重新分组。
2. 重复顺播、远距离拖动、快速连续拖动和播放中关闭视频。记录成功拖动的 `active_ms` 的 P50/P95/最大值，同时报告中止请求和样本数。样本不足时保留原始结果，不把少数更快的拖动当作长尾改善。
3. 对照同等播放时长下的卡顿次数/总时长，以及 `remote_payload_bytes` 的差值；分别比较重复接收和退出时的 `remote_unread_bytes`。
4. 若 `waiting=0` 后仍出现 `pressure_requested=0 pressure_local=1`，结合 `pressure_age_ms`、限流状态和吞吐，确认是否留下了过时压力。这批诊断不改变现有压力解除规则。

关闭开关后，新采集器只读取原子开关状态，不进行逐分片区间统计或字符串格式化；统计状态集中在 `media_streaming_diagnostics.cpp`，主播放路径只调用事件接口。
