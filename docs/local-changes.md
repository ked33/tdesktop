# 当前本地功能资产

本文件记录当前代码树仍有入口或仍影响构建/产品身份的本地改动。本轮代码树是独立升级分支的合并索引，尚未创建合并提交。它不是提交清单；全部本地非 merge 提交见 [local-changes-history.md](local-changes-history.md)。

文档基线快照（2026-09-14，v7.2.8 合并候选）：

```text
branch: upgrade/v7.2.8
source_head: efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11
code_tree: resolved merge index, not committed
candidate_official_parent: 272f6f5c2d29d8cdb3aec15907d616b87451a3ca (v7.2.8)
last verified official merge commit: e800800df3119657b2d95289cb060cd67bdba583
last verified official parent: 2f41383dddd338fe17fd4711afd02688c418fd47 (v7.2.5)
local non-merge author commits reachable from source_head: 330
```

2026-09-11 整理时，将根目录 `我的修改.md`（原目录的忽略文件） 的第 1–18 节按主题补入正文和历史附录，并核对本地 Git 提交、直接源码入口和设置默认值。原文第 1–12 节的 144 / 138 个提交是旧 `--all` 范围快照；第 13–18 节已更新到 2026-09-11，不能把整份原文视为过期。原文被 `.gitignore` 忽略，该次保留原文件。

该基线不替代下次升级时重新计算 merge-base 和 official anchor，也不表示所有历史功能已经重新运行验证。2026-09-11 的整理只做文档和静态核对，未编译、运行客户端或查询 Actions 结果。正文中的“固定回归 / 回归重点”是后续验证清单。

2026-09-12 定向补录：核对 `937731a`～`210db5e`（含首尾，共 6 个提交），将被标记聊天的补充搜索、独立“特殊搜索”、并发与请求间隔、固定总数和计时进度、限流提示与日志、黄色 `18+` 标记补入第 8、11.3、12.1 节。补录代码截至 `210db5e47910d080de22f5772f39b5f5da4e5f5f`；上方官方基线和 311 条历史统计仍为 2026-09-11 快照，搜索云验证记录见第 12.1 节。

2026-09-14 定向补录：核对 `0812c59`～`efa2a4c`（含首尾，共 12 个提交），按最终实现更新第 1.3、1.6、8.3、12.1 节，并新增第 1.8 节的分片 MP4 按需索引与回跳加载、第 8.5 节的固定搜索进度与结果导航、第 10.1 节的聊天选择器搜索。补录代码截至 `efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11`；上方官方基线和历史统计保持原快照。本次只做提交、源码和文档静态核对，未编译、运行测试或客户端，也未查询新的 Actions 结果。

2026-09-14 v7.2.8 合并更新：F01–F08 已按用户批准的方案落实，保留 64Gram 公开版本 7.0.9，候选 `UpstreamVersion=7.2.8`。目录搜索、手势、RPC、媒体缓冲和实验滚动设置已适配，配套子模块同步到目标。历史附录更新为 330 条。上面两次“定向补录”中的 311 条属于各自当时的旧快照；本轮证据和未验证项见 [升级记录](upstream-merges/v7.2.8/README.md)。

## 1. 在线播放、MPV 与流媒体状态机

用户能力：消息右键和鼠标快捷方式调用普通/特殊 MPV 在线播放；支持自定义 `mpv.exe` 路径、调试日志、不同加载档位、Smart 自适应策略、远距离 seek 取消、MP4 tail 预取、高码率提示，以及内置播放器的 Soft Seek/seek map/缓存复用与资源生命周期修复。

关键入口：

- `Telegram/SourceFiles/media/streaming/media_streaming_mpv.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_mpv.h`
- `Telegram/SourceFiles/media/streaming/media_streaming_mpv_special.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_mpv_special.h`
- `Telegram/SourceFiles/media/streaming/media_streaming_mp4_fragment.h`
- `Telegram/SourceFiles/media/streaming/media_streaming_mp4_index.h`
- `Telegram/SourceFiles/media/streaming/media_streaming_mpv_index.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_mpv_metadata.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_reader.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_file.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_source.cpp`
- `Telegram/SourceFiles/media/streaming/media_streaming_boost.cpp`
- `Telegram/SourceFiles/storage/download_manager_mtproto.cpp`
- `Telegram/SourceFiles/history/view/history_view_context_menu.cpp`

设置与资源：`net_download_speed_boost`、`net_download_speed_boost_profiles`、`mpv_path`、`mpv_streaming_debug_logs`、`online_playback_debug_logs`、`show_message_context_stream_in_mpv*`，以及 `lng_settings_mpv_debug_logs`、`lng_settings_online_playback_*` / `lng_online_playback_*`。

代表提交：`9a4df4e80b`、`05b9fade73`、`365fad7691`、`7f46ca74ce`、`25c9fa8163`、`cf924df11d`、`e72b78429a`、`0441bc0062`、`e3e7c64fba`。

上游合并高风险：FFmpeg/AVIO API、Reader/Loader 请求模型、播放线程 stop/join、EOF/InvalidData、stale frame timer、重复 part delivery、下载器并发和 CMake 源文件清单。

固定回归：普通/特殊 MPV 首播、短/长 seek、暂停恢复、连续 seek、播放结束、关闭窗口/退出进程；Smart 档位高码率、服务端限速和缓冲压力；Soft Seek 的重复 part、EOF、取消请求、seek map 构建期间远端读取；mpv PATH 回退和日志开关。

### 1.1 MPV 入口、路径与快捷操作

- 普通 `mpv 播放` 与 `MPV 播放（特殊）` 均仅在 Windows 提供；特殊入口还需按住 `Ctrl` 右键消息，并开启对应菜单开关。普通入口覆盖更多可在线播放或可转换为流式加载的媒体。
- `Ctrl + 左键` 触发 MPV 播放，`Alt + 左键` 复制消息链接。`mpv_path` 留空时使用系统 `PATH`，填写时优先调用指定 `mpv.exe`。
- 两条 MPV 路径分别保留兼容处理、远距离 seek 取消和 MP4 tail moov 预取；日志开关默认关闭，当前共同诊断范围见第 1.7 节。
- 早期独立 AVIO 直连方案的实验和回退见历史附录，不能把当时入口当作当前独立功能；现有内置播放器的 FFmpeg/AVIO、Soft Seek 和缓存修复仍按各自代码入口维护。

代表提交：`9a4df4e80b`、`2e713eba49`、`8051d4e31d`、`790f8b04bd`、`05b9fade73`。

第 1.2–1.7 节以 `a5b7115294`～`797ad28e62` 的补录为基础，其中第 1.3、1.6 节已按本轮 MPV 提交更新，分片索引与回跳行为另见第 1.8 节。“智能档”策略适用于非 Premium 账号的远程流媒体播放，需要将 `设置 → 增强设置 → 网络 → 优化在线播放` 设为 `智能（6）`；原画开关、日志采集、MPV 格式兼容及通用缓存修复按各自条件生效。

### 1.2 内置播放器默认播放原画质

新增 `设置 → 增强设置 → 网络 → 内置播放器默认播放原画质`，配置键为 `video_player_prefer_original`，默认开启；旧配置缺少此键时也补为开启。

- 打开视频时选择原文件，以固定原画模式开始播放，自动降画质请求不会覆盖该选择。
- 播放中仍可手动切换画质；开关在下次打开视频时生效，无需重启客户端。
- 关闭后恢复原有画质选择逻辑；本地已有原文件时仍保留原先优先原画的行为。
- 自动画质模式下，因持续缓冲而产生的降画质请求也统一受冷却限制，减少重复请求；日志分别记录请求、忽略和实际切换。
- 同步补充简体中文、英文界面名称及说明，提示原画播放可能增加流量消耗。

主要提交：`8876427168`、`aad1c1f633`。

### 1.3 MPV 大文件头、多数据块 MP4 的首播与拖动

同时调整普通 `mpv 播放` 和 `MPV 播放（特殊）` 两条路径。

- 大型前置 `moov`（MP4 索引）恢复按请求区间响应，并使用独立 Reader 处理非零位置读取；移除先顺序读取一定数据量、达到特定偏移后才允许跳转的旧限制，减少首播探测和拖动进度互相干扰。
- MP4 启动参数加入 `fflags=+ignidx`，避免解复用器为扫描后续 `mdat`（媒体数据块）而遍历远程文件，保留利用 `moov` 采样表定位的能力。
- 共用 MP4 文件头探测与修补逻辑。对已确认具有前置索引的非分片 MP4，将提供给 MPV 的第一个 `mdat` 长度覆盖到文件尾，修复多个数据块之间跳转时再次扫描的问题；修补只作用于输出缓冲，原文件和采样偏移保持不变。
- 头部不完整、结构异常或不能确认布局时不应用上述长度修补；分片 MP4 也不使用这一非分片 `mdat` 修补，当前改为正常 Range 响应与索引辅助跳转，见第 1.6、1.8 节。

主要提交：`a5b7115294`、`f03c85a11b`、`89a2be9e45`。

核对来源：[普通 MPV 桥接](../Telegram/SourceFiles/media/streaming/media_streaming_mpv.cpp)、[特殊 MPV 桥接](../Telegram/SourceFiles/media/streaming/media_streaming_mpv_special.cpp)、[MP4 文件头处理](../Telegram/SourceFiles/media/streaming/media_streaming_mp4_header.h)。

### 1.4 智能档首播缓冲、拖动进度与定向预读

- 视频已就绪后，以约 1.5 秒可播放缓冲为启动目标；首播额外等待达到 1.5 秒、拖动后达到 1 秒时，将缓冲门槛放宽为 0.5 秒。所需媒体缓冲按播放倍速折算，并正确区分画面就绪、仍在缓冲和实际开始播放。
- 拖动时优先利用 MP4 采样表或 FFmpeg 已有索引，预取关键视频数据及对应音频；无法可靠定位时，根据随后实际读到的媒体包位置建立预读窗口。
- MP4 预读按实际数据块分别规划，处理音视频交错、数据块间距较大或存放顺序不同的文件；限制区间数、总字节数和关键分片数，避免把中间大段无关数据一起预取。
- 当前解码真正缺少的数据优先于推测性的预读；关键数据先尝试加载缓存，连续拖动时丢弃过期预取计划，并记住已经完成的关键分片，减少旧任务和重复请求阻塞新位置。
- 启动时识别实际 `moov` 范围，为不超过 8 MiB 的索引按范围预读，并受请求并发上限约束，减少文件头逐小块等待和越界预取。

主要提交：`8876427168`、`90e017416d`、`aad1c1f633`。

核对来源：[播放器缓冲](../Telegram/SourceFiles/media/streaming/media_streaming_player.cpp)、[启动缓冲策略](../Telegram/SourceFiles/media/streaming/media_streaming_startup.h)、[解复用与拖动](../Telegram/SourceFiles/media/streaming/media_streaming_file.cpp)、[MP4 预读区间](../Telegram/SourceFiles/media/streaming/media_streaming_mp4_seek.h)。

### 1.5 弱网恢复、关键读取调度与缓存

智能档新增或完善以下行为：

- 单独跟踪当前播放所需数据的等待时间；即使其他请求仍在返回数据，也能识别关键读取停滞。无进展时清除过时吞吐和突发预读估计，收敛预读，并唤醒等待方更新状态。
- 对长期无响应的关键请求，按延迟与抖动计算 4–10 秒重试门槛，尝试使用其他可用下载会话重发；保留冷却和次数限制，重试后继续正确记账。
- 当关键分片仍排在队列中、并发名额被过期的非必需请求占用时，可替换一个符合条件的旧请求，把名额交给当前读取。
- 上述主动重试和替换会避开 CDN、服务端限速及恢复期，并尊重协议层已安排的延迟，避免反复抢发。
- 修复缓冲压力在最短保持时间过后无法释放的问题；增加定时解除，停止播放时清理当前读取需求。
- 智能档保留的内存数据切片从 2 个增加到 4 个，减少音视频交错读取时频繁换入换出。

缓存通用修复包括：数据切片被回收后，可从已保留的文件头分片恢复相应内容；跨 8 MiB 切片边界时共享同一份预读预算，按实际读取终点分配到后续切片，避免边界缺口和重复放大预读量。

主要提交：`8876427168`、`70f630ea15`、`90e017416d`、`aad1c1f633`、`666741cddb`。

核对来源：[Reader 与缓存调度](../Telegram/SourceFiles/media/streaming/media_streaming_reader.cpp)、[读取停滞策略](../Telegram/SourceFiles/media/streaming/media_streaming_read_stall.h)、[MTProto 加载器](../Telegram/SourceFiles/media/streaming/media_streaming_loader_mtproto.cpp)、[下载请求重试与替换](../Telegram/SourceFiles/storage/download_manager_mtproto.cpp)、[缓存策略](../Telegram/SourceFiles/media/streaming/media_streaming_cache.h)。

### 1.6 MPV 的 Range 请求与断连取消

普通 MPV 桥接由 `797ad28e62` 调整请求取消；`2027b874a3` 进一步将可取消读取用于特殊 MPV，并移除两条路径对分片 MP4 的顺序响应限制。当前行为如下：

- 普通桥接移除仅根据 HTTP Range 偏移跳变推测用户拖动、再额外触发 seek 预取和缓冲压力的逻辑，减少 MPV 探测请求引起的下载窗口反复切换。
- 普通与特殊桥接均使用 64 KiB 读取块；分片 MP4 的部分请求按实际区间返回 `206`，不再忽略 Range、从文件头返回整份内容。大型前置 `moov` 和分片 MP4 的非零位置读取使用独立 Reader。
- 等待数据期间检测连接断开、播放器退出及较新的隔离 seek 请求；旧请求可中止等待，并清理读取通知和需求，让后续请求继续处理。
- 两条桥接均移除客户端断连后继续顺序填充缓存的循环，避免旧请求占用读取锁并与新位置争用下载窗口。

主要提交：`797ad28e62`、`2027b874a3`。

核对来源：[普通 MPV 桥接](../Telegram/SourceFiles/media/streaming/media_streaming_mpv.cpp)、[特殊 MPV 桥接](../Telegram/SourceFiles/media/streaming/media_streaming_mpv_special.cpp)、[可取消读取](../Telegram/SourceFiles/media/streaming/media_streaming_mpv_http.h)。

### 1.7 播放卡顿、拖动延迟与下载量诊断

在原有日志开关下新增统一诊断统计，覆盖内置播放器、Reader、MTProto 下载及普通 / 特殊 MPV 桥接。两个开关均默认关闭；开启 `online_playback_debug_logs` 或 `mpv_streaming_debug_logs` 任意一个即可启用共同统计，两者都关闭后停止共同采集。

- 通过播放 ID、Reader ID、采集阶段和拖动代次关联事件，区分启动、连续拖动、切换视频与中途中止。
- 记录画面就绪、可开始播放、实际帧呈现的耗时，以及用户暂停时间、缺数据卡顿次数和时长、音视频缓冲；后续补充解复用包、索引定位和关键读取计划。
- 统计实际接收、重复覆盖、已供应、尚未读取、缓存复用等字节量，并记录排队 / 派发等待、取消、重试、预读与限速状态。
- MPV 桥接记录首个响应块完成、读取锁等待、数据填充、成功写出、断连和请求被替代等信息；HTTP 传输耗时与播放器首帧呈现耗时分别解释。
- 支持即时切换采集；中途开启的片段会标记为不完整采样。文件区间和请求关联表均设有数量上限，关闭后跳过逐分片统计及相应字符串格式化。

主要提交：`1c38ad28ab`，后续由 `8876427168`、`70f630ea15`、`90e017416d`、`aad1c1f633` 补充统计口径与观察点。

核对来源：[诊断实现](../Telegram/SourceFiles/media/streaming/media_streaming_diagnostics.cpp)、[基础日志字段说明](online-playback-diagnostics.md)。日志用于分析实际卡顿和下载开销，不能仅凭新增统计断言播放性能提升。

补充回归重点：大于常规探测窗口的前置 `moov`、多个 `mdat` 与分片 MP4；弱网关键读取停滞、限速/CDN 下的重试边界；跨 8 MiB 切片读取；64 KiB Range 短探测、断连和旧请求取消；默认原画与手动画质切换；日志即时开关及不完整采样。

### 1.8 分片 MP4 的按需索引、缓存外跳转与回跳加载

普通与特殊 MPV 共用索引准备及 IPC 重载控制。首播继续使用 `ignore_editlist=1,fflags=+ignidx` 快速打开；需要跳转到未缓存位置时，优先利用目标附近的分片元数据，避免每次都等待全文件扫描。

- 独立 Reader 读取并缓存索引元数据，专用 `Metadata` 模式关闭额外预读、最多使用 2 个并发请求，减少索引准备与媒体读取争用。分片文件头中为零或未知的 `mvhd` / `mdhd` 时长可按 Telegram 提供的有效时长补齐，仅修改发送给 MPV 的缓冲。
- 对没有全局索引、时长已知且可解析的分片 MP4，按目标时间探测附近 `moof`，解析音视频时间及采样信息，选择可独立解码的视频分片并兼顾音频。处理空轨道分片、B 帧呈现时间偏移、大 `mdat` 内重复探测和声明时长超出实际文件尾等情况；读取量、迭代次数和缓存定位点均有上限。
- 每次定位生成独立的 `?tdesktop_index=<revision>` 视图，把目标之前的分片区间在输出中包装为 `free` atom，使 MPV 从目标附近建立局部索引。媒体偏移、文件总长度和原文件保持不变；最多保留 8 份视图，正在发送的 HTTP 响应持有自己的固定快照。
- 缓存内正常跳转优先交给 MPV；已确认目标早于当前视图起点时，立即重新打开目标视图，HTTP 响应等待该索引就绪。等待中保持加载状态，避免索引尚未完成就恢复到旧视图起点，造成进度回弹和错误位置继续播放。
- 控制器结合已识别的 MPV 命令 / 执行日志、位置通知和查询响应保留真实目标，避免合并通知或晚到的属性响应覆盖原目标。同一目标复用在途索引并去重重载，新目标可接管旧定位；自身索引视图加载期间支持可解析的绝对、百分比和相对跳转，相对跳转以等待中的目标为基点。
- 重载保留用户当前暂停 / 继续状态，区分 EOF 自动暂停；临时启用的 idle 在控制结束或切换文件后恢复。显式停止、退出、连接断开或目标过期会取消相应等待，索引 Reader 在主线程停止并释放。
- 已有全局索引、未知时长或无法可靠解析的布局保留完整元数据扫描回退，仍可能有较长等待。目标日志依赖已识别的 MPV 文本格式；未知格式继续使用属性路径，不能据此保证所有 MPV 版本的通知合并问题均已覆盖。

沿用第 1.7 节的播放日志开关，新增 `MPV controller` JSON 和索引 requested / started / cancelled / settled 记录，关联输入目标、执行目标、位置回复、完成判定、重载、目标序号和 revision，并补齐索引取消原因、读取量和耗时。诊断记录覆盖关键状态转换，不逐次记录正常播放的位置更新。

主要提交：`2027b874a3`、`db21ec8d53`、`f107726904`、`a84a5a0c9c`、`788b38b592`、`cb851f26e9`、`41f0ac1136`。

核对来源：[分片定位](../Telegram/SourceFiles/media/streaming/media_streaming_mp4_fragment.h)、[完整元数据缓存](../Telegram/SourceFiles/media/streaming/media_streaming_mp4_index.h)、[索引准备与快照](../Telegram/SourceFiles/media/streaming/media_streaming_mpv_metadata.cpp)、[IPC 目标与重载控制](../Telegram/SourceFiles/media/streaming/media_streaming_mpv_index.cpp)、[专项说明与既有验证记录](mpv-fragmented-seek-fix.md)。新增源码已接入 [Telegram 构建清单](../Telegram/CMakeLists.txt)，测试配置见第 12.1 节。

回归重点：无索引分片 MP4 的冷前跳 / 冷回跳、连续 exact / keyframes 输入、位置通知合并、等待期间重复或更换目标、暂停后继续、EOF / stop / quit、切换文件与取消读取；同时覆盖普通 MP4、大 `moov`、多 `mdat`、尾部 `moov`、全局索引及 B 帧布局。回跳需检查整个过程是否曾回到旧进度，不能只检查最终落点。

## 2. 在线播放参数 Profiles 与非 Premium 限速

增强设置提供多档在线播放 profile 编辑器，分组维护 Reader、下载 session、MPV cache 和 Smart 参数，并做上下界校验。`FLOOD_PREMIUM_WAIT_x` 可按本地配置覆写等待时间，下载/上传链记录限速信息。

关键入口：`Telegram/SourceFiles/boxes/enhanced_options_box.cpp`、`Telegram/SourceFiles/settings/settings_enhanced.cpp`、`Telegram/SourceFiles/core/enhanced_settings.cpp`、`Telegram/SourceFiles/storage/storage_non_premium_delay.h`、`Telegram/SourceFiles/storage/file_upload.cpp`、`Telegram/SourceFiles/mtproto/mtp_instance.cpp`。

设置：`net_download_speed_boost_profiles`、`flood_premium_wait_override_ms`。代表提交：`2191fe55e6`、`495774948c`、`7f46ca74ce`、`bd586b8200`、`1d6366fb5b`。

固定回归：各 profile 独立保存/重置、非法整数和参数范围、重启后读取、Smart 专属参数、下载/上传限速重试、负数/空值默认行为。

补充行为：

- “优化在线播放”最初名为“下载速度提升”，当前有已禁用、轻度、中等、高、激进、极限、智能七档（0–6），默认已禁用。上传速度提升另用 `net_speed_boost`，有 0–3 四档。
- 参数弹窗包含 22 个数值项与 2 个开关；编辑档位与实际启用档位分开，切换时校验并暂存，保存全部档位，重启后应用。范围、跨字段约束及还原行为见第 11.2 节。
- 非 Premium 等待覆写留空时采用服务端等待时间，非负整数按毫秒解释，0 表示不额外等待。当前请求处理时直接读取配置，新遇到的限速事件可使用新值；它不解除服务端限速。
- 下载和上传 toast 分别在 `SessionController::checkNonPremiumLimitToastDownload` / `SessionController::checkNonPremiumLimitToastUpload` 记录 `FLOOD_PREMIUM_WAIT_x`，便于区分限速、客户端重试等待和播放链路问题。

## 3. Google 翻译后端、缓存与格式保护

默认聊天翻译后端可选 Google/MTProto；Google 路径包含 LRU 缓存和链接/代码保护，减少重复请求并避免翻译破坏 URL、代码块或受保护格式。

关键入口：

- `Telegram/SourceFiles/lang/translate_backend.cpp`
- `Telegram/SourceFiles/lang/translate_google_provider.cpp`
- `Telegram/SourceFiles/lang/translate_cache.cpp`
- `Telegram/SourceFiles/lang/translate_protect.cpp`
- `Telegram/SourceFiles/lang/translate_provider.cpp`
- `Telegram/SourceFiles/history/view/history_view_translate_tracker.cpp`
- `Telegram/SourceFiles/boxes/language_box.cpp`
- `Telegram/CMakeLists.txt`

设置：`translation_provider`、`translation_keep_protected_format`、`translate_to_tc`。代表提交：`0f36f3dc54`、`d44bde62bb`。

上游合并高风险：上游翻译 provider 接口、请求 callback 生命周期、CMake 源文件清单、语言设置 UI、聊天自动翻译 tracker 和 Telegram 自身 Cocoon/平台翻译演进。

固定回归：单条/整聊翻译、Google 与 MTProto 切换、缓存命中/淘汰、同文不同目标语言、URL/Markdown/代码块/自定义 emoji、失败回退、快速切换聊天和关闭窗口后的 callback 安全。

## 4. 消息菜单、详情、快速操作和聊天菜单

本地为消息右键菜单增加 Details、MPV、Repeater 等入口和逐项可见性开关；调整 Copy Post Link 顺序；增强聊天空白区、右上三点和聊天列表群组菜单；支持 Alt+左键复制链接、Ctrl+左键播放。

关键入口：`Telegram/SourceFiles/history/view/history_view_context_menu.cpp`、`Telegram/SourceFiles/history/history_inner_widget.cpp`、`Telegram/SourceFiles/window/window_peer_menu.cpp`、`Telegram/SourceFiles/window/window_session_controller.cpp`。

设置：`show_message_context_*`、`show_json`、`show_repeater_option`、`repeater_reply_to_orig_msg`。代表提交：`77d84767e2`、`5bd6e9cf2b`、`129e68c89a`、`226e432a9f`、`69d2288ff1`。

固定回归：普通消息、相册、服务消息、受保护内容、空白区、无 item、选择态、评论线程、频道和群；每个开关隐藏/显示是否正确；右键崩溃、空指针和菜单生命周期。

补充菜单语义：

- `Details / 详情` 包含浏览量、转发数、消息 ID、发送/编辑/转发时间，以及适用媒体的大小、MIME、文件名、分辨率、数据中心、视频编码、清晰度、表情或贴纸包作者。
- 21 个顶层通用菜单开关默认全部开启，完整键名与条件见第 11.4 节；复读机仍需同时开启 `show_repeater_option`，特殊 MPV 仍需 `Ctrl + 右键`。
- `Copy Post Link` 移到菜单靠前位置。聊天空白处增加 `Add to folder`、`Clear history`、`Go to first message`、`Go to random ID message`、`Archive`。
- 对话右上三点菜单增加分组，在“跳转到第一条消息”下提供随机 ID 跳转：先获取最大 ID，再跳到随机 ID 附近。聊天列表群组右键另有“删除我的消息”。

补充代表提交：`6c0605e2f3`、`2899cd94bf`、`7b8b023672`、`0b1b1ae992`、`a95b9a9229`。随机 ID 不一定对应现存消息，回归时需覆盖删除消息后的 ID 空洞和权限限制。

## 5. 跨聊天选择、拖选、评论线程与快速复制

支持跨聊天保留选择、跨聊天统计/转发、评论与回复线程选择、拖选预览和数量上限、相册组选择、selected-copy 适配，以及把选中消息以无引用转发方式快速复制到预设目标；满足成功条件时还会尝试删除可删除的源消息。

关键入口：`Telegram/SourceFiles/data/data_session.cpp`、`Telegram/SourceFiles/data/data_session.h`、`Telegram/SourceFiles/history/history_inner_widget.cpp`、`Telegram/SourceFiles/history/history_widget.cpp`、`Telegram/SourceFiles/history/view/history_view_list_widget.cpp`、`Telegram/SourceFiles/history/view/history_view_top_bar_widget.cpp`。

设置：`keep_selected_messages_across_chats`、`quick_copy_targets`。代表提交：`12daeb1a46`、`3043740398`、`663c91533d`、`fd6ea2193f`、`b35fc392d8`。

上游合并高风险：`SelectedItems` 类型、album/group selection、History 和 ListWidget 双实现、rich-copy/wrapped text、评论 thread item identity、selection limit 和 top bar 状态。

固定回归：跨两个聊天选择/取消；评论、回复、相册、全局搜索结果；点击空白不生成零长度选择；快速滚动拖选、达到上限、反向取消；复制、转发、删除源消息和失效目标。

`quick_copy_targets` 用英文逗号分隔目标，默认 `-1002615379741,Saved Messages` 表示两个目标，留空停用；目标支持用户 ID、普通群负 ID、频道/超级群 `-100...` ID 和不区分大小写的 `Saved Messages`。其他目标需已被当前会话加载；重复目标去重，无效或不可发送目标跳过，没有可用目标时提示先打开目标对话并检查设置。

**删除原消息没有独立开关，也不会再次确认。** 只有全部目标转发成功、没有目标被跳过且所选内容不含已保存音乐时，才对有权删除的原消息请求为所有人删除；最终仍受服务端权限约束。完整设置说明见第 11.3 节。关闭跨聊天保留选择时，会清空已保存的跨聊天选中状态。

补充代表提交：`ee80b51614`、`3076acc3db`。固定回归补充：默认两个目标、空配置、去重/跳过、部分转发失败、保存音乐、删除权限和自动删除条件。

## 6. Compose 编辑消息导航与快捷键

编辑消息时使用 `Alt+Shift+Up/Down` 在可编辑消息之间导航，并保持输入框已有文本中的普通光标移动；同时覆盖新的 `HistoryView::ComposeControls` 与 legacy `HistoryWidget` 路径，不改变 Ctrl+Up/Down 的回复目标语义。

关键入口：`Telegram/SourceFiles/history/view/controls/history_view_compose_controls.cpp`、`Telegram/SourceFiles/history/history_widget.cpp`、`Telegram/SourceFiles/core/shortcuts.cpp`。

代表提交：`a552560bf2`、`55e7ec095f`、`c5333f47fb`、`7fab867a04`、`9183ffce47`。

固定回归：普通主聊天、scheduled/settings 等新 Compose 路径；空/非空输入框、首尾消息、不可编辑消息、快捷键冲突、Ctrl+Up/Down 回复导航。

## 7. 媒体查看器、预览亮度与播放器快捷键

聊天预览图支持亮度调节；媒体查看器按区域用滚轮控制亮度、音量、切换和 seek，并在重新打开时重置状态；视频播放器增加速度、旋转、镜像、受限频道/群视频帧复制，以及正文隐藏和播放控件布局调整，高码率视频大小可显示提示标记。

关键入口：`Telegram/SourceFiles/history/view/media/history_view_preview_brightness.cpp`、`Telegram/SourceFiles/history/view/media/history_view_preview_brightness.h`、`Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp`、`Telegram/SourceFiles/media/view/media_view_overlay_renderer.h`、`Telegram/SourceFiles/media/view/media_view_overlay_opengl.cpp`、`Telegram/SourceFiles/media/view/media_view_overlay_raster.cpp`、`Telegram/SourceFiles/media/view/media_view_playback_controls.cpp`。

设置：`preview_brightness_enabled`、`preview_brightness`、`media_viewer_wheel_control_enabled`。代表提交：`7303113612`、`55dd0a6c25`、`672beb04aa`、`6256411a4a`、`8042268e0e`。

固定回归：单图/相册/GIF/视频预览；OpenGL/Raster；不同界面缩放；滚轮四区域、OSD、重开重置、视频双击、数字键/长按方向键/PageUp。

### 7.1 预览亮度、滚轮分区与键盘控制

- 聊天预览亮度只作用于静态图片、GIF/视频缩略图和相册预览，不改变实际播放或媒体查看器亮度；默认关闭，亮度默认 70%，可选 10%–100%，步长 10%。
- 查看器滚轮增强默认关闭。启用后横向分为三等分：左区上半调亮度、下半调音量，中间切换媒体，右侧对视频 seek；亮度/音量有 OSD 提示。重新打开时重置亮度与音量，不持久化查看器亮度。
- 数字键 `2` 为 `0.5x`、`3` 为 `2x`；长按右方向键使用 `Media::kSpeedMax` 的 `2.5x`，`PageUp` 旋转视频。
- 视频/GIF 信息展示、播放器时长、高码率提示与缩放布局需一起回归。

补充代表提交：`1dc38bcc89`、`ac68ee8d45`、`1a1a6f68e2`、`f2dd568e74`、`a95b9a9229`、`875b827372`、`037e9f72d5`。

### 7.2 内置媒体查看器镜像翻转

提交：`de42a18855`（Add horizontal mirroring to the media viewer）。

入口：内置媒体查看器右下角三点按钮的悬停菜单，在 `显示所有文件` 下方新增 `镜像翻转`；照片对应 `显示所有照片`。媒体右键菜单复用同一入口列表。

- 点击后将当前图片或视频按画面水平方向镜像翻转，菜单右侧开关显示启用状态；再次点击恢复。默认不镜像，无需开启增强设置。
- 覆盖普通照片、作为文件发送的图片、视频及动图；加载中的视频也可提前切换，后续画面沿用当前状态。
- 镜像可与旋转、缩放和全屏查看配合使用，旋转后仍按画面水平方向翻转；暂停、拖动进度和切换当前视频画质时保留镜像状态。
- 切换到其他媒体或关闭查看器后重置；同一媒体的重新排版、旋转或刷新不会清除状态，也不会重写原媒体文件。
- 图片和视频统一在渲染时翻转，覆盖 OpenGL、RHI 和软件渲染；拖动进度时的临时封面不会重复翻转，GPU 视频路径无需逐帧生成镜像位图。
- 已有 `H` / `V` 翻转快捷键复用同一状态和处理逻辑；文字识别高亮及鼠标选区同步映射到翻转后的画面。
- 新增语言键 `lng_mediaview_flip_horizontal`，简体中文为 `镜像翻转`，英文为 `Flip Horizontally`，复用已有翻转图标。

核对来源：[菜单、状态和坐标映射](../Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp)、[内容几何结构](../Telegram/SourceFiles/media/view/media_view_overlay_widget.h)、[OpenGL 渲染](../Telegram/SourceFiles/media/view/media_view_overlay_opengl.cpp)、[RHI 渲染](../Telegram/SourceFiles/media/view/media_view_overlay_rhi.cpp)、[软件渲染](../Telegram/SourceFiles/media/view/media_view_overlay_raster.cpp)。

回归重点：照片、文件图片、视频及动图，加载中切换、旋转后水平翻转、缩放/全屏、暂停/拖动/切画质保留状态、换媒体/关闭重置，OpenGL/RHI/软件渲染和文字识别选区。

### 7.3 禁止转发频道和群组的视频帧复制

提交：`25da08c25b`（Optimize media player layout）。

入口：在禁止转发的频道、普通群组和超级群组中打开视频，画面就绪后，内置媒体查看器右下角三点悬停菜单和媒体右键菜单恢复显示 `复制当前帧`。

- 同时修正菜单显示条件和复制处理函数，点击后将当前视频帧作为图片写入剪贴板。
- 复制当前显示的视频帧，保留旋转和镜像翻转效果；播放与暂停状态均可使用。
- 视频帧复制采用独立图片路径，初始加载尚未显示视频画面时不会开放受限媒体复制；普通图片、文件保存及其他内容类型沿用原有处理。
- 频道和群组中的视频帧复制无需额外开关；复用现有菜单文案和复制图标。

核对来源：[复制条件、菜单与剪贴板处理](../Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp)、[接口声明](../Telegram/SourceFiles/media/view/media_view_overlay_widget.h)。

回归重点：频道、普通群和超级群，初始加载与已有视频帧，播放/暂停，旋转和镜像后的剪贴板图片；普通图片、文件保存及其他受限内容保持原条件。

### 7.4 隐藏媒体消息正文、标签和广告文案

提交：`25da08c25b`（Optimize media player layout）。

- 普通媒体查看器打开视频或图片时，隐藏画面及播放控件附近的消息正文，包括 `#xxx` 标签、链接和一般说明文字。
- 同步清除正文的布局和鼠标命中区域，隐藏的标签、链接不会继续响应点击。
- 继续从消息正文解析时间戳和章节，保留章节名、进度条分段和章节跳转；操作提示、播放时间也保留。
- 打开媒体、切换媒体、切换画质时统一生效；Stories 使用原有说明文字显示逻辑。
- 视频播放广告隐藏标题与正文，保留缩略图、广告信息及关闭控件；广告请求和原有曝光上报链路继续执行。
- 真实点击广告区域时继续发送点击上报，随后不再打开广告链接或频道；没有新增自动点击或定时点击上报。

核对来源：[消息正文、章节解析与显示布局](../Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp)、[播放时间与章节控件](../Telegram/SourceFiles/media/view/media_view_playback_controls.cpp)。

视频广告来源核对：独立广告组件 `PlaybackSponsored` 由上游 `ecc955d2ce`（2025-07-11）实现，`78e78f57d8`（2025-07-15）已将其合入本分支历史。最近的 `e800800df3`（2026-09-06，合并 v7.2.5）未新增该组件或改变视频广告触发条件；该组件接收服务器返回的广告并显示在播放控件上方，本次保留组件并关闭标题、正文绘制和广告链接跳转。核对来源：[视频广告组件](../Telegram/SourceFiles/media/view/media_view_playback_sponsored.cpp)、[视频广告请求与上报](../Telegram/SourceFiles/data/components/sponsored_messages.cpp)。

回归重点：普通图片/视频的正文与鼠标命中区域、媒体/画质切换、章节解析及跳转、Stories 说明文字，广告缩略图/信息/关闭控件及真实点击上报。

### 7.5 播放时间贴近播放按钮与进度条扩宽

提交：`25da08c25b`（Optimize media player layout）。

- 将进度条左侧的当前播放时间移动到 `播放/暂停` 按钮左侧，右侧的总时长移动到按钮右侧；两侧文字与按钮垂直居中，并使用随界面缩放调整的紧凑间距。
- 进度条向左右扩宽，填补原先时间标签及标签与进度条之间的空位，仅保留控件左右外边距；时间文字变长时不再挤压进度条。
- 下载百分比改为在总时长与全屏按钮之间居中，避免与移位后的时间文字重叠；从右向左语言同步调整布局。
- 章节名称继续与进度条对齐，章节分段、跳转、拖动时的时间更新及播放操作提示沿用原有行为。

核对来源：[时间、进度条及下载进度布局](../Telegram/SourceFiles/media/view/media_view_playback_controls.cpp)、[播放器间距样式](../Telegram/SourceFiles/media/view/media_view.style)。

回归重点：不同界面缩放、长时长、窄窗口、从右向左语言、下载百分比、章节名称/分段、拖动时间更新及操作提示。

## 8. 对话列表、搜索和视觉主题

对话头像显示 no-forwards 标记并允许主题色；搜索结果双击复制链接；全局搜索可包含归档、补搜被标记 `porn` 的频道和群组，也可通过“特殊搜索”单独检索这些聊天；搜索结果头像 / 图片增加黄色圆形 `18+` 标记。另支持搜索消息高亮色、代码块背景色、隐藏 Stories/All Chats、截图隐私、群组发送者头像等增强选项。

关键入口：`Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp`、`Telegram/SourceFiles/dialogs/dialogs_row.cpp`、`Telegram/SourceFiles/dialogs/dialogs_row.h`、`Telegram/SourceFiles/dialogs/ui/dialogs_layout.cpp`、`Telegram/SourceFiles/dialogs/dialogs_widget.cpp`、`Telegram/SourceFiles/ui/chat/chat_style.cpp`。

设置：`no_forwards_badge_color`、`double_click_copy_link`、`search_main_and_archive`、`search_include_porn`、`search_porn_concurrency`、`search_porn_request_interval_ms`、`search_message_highlight_bg_color`、`code_block_bg_color`、`hide_stories`、`hide_all_chats`、`screenshot_mode`、`show_group_sender_avatar`。

代表提交：`8489fed679`、`f0d3732348`、`125fbe3c3f`、`76d067f8cd`。固定回归：普通/hover/active 主题态，受保护与非受保护对话，归档搜索开关，主题切换、缩放和高 DPI。

no-forwards 徽标使用透明背景，实际绘制颜色由 `no_forwards_badge_color` 覆盖，默认 `#ecbb71`；新增 `18+` 圆形底色共用该配置，尺寸和对齐见第 8.4 节。代码块默认 `#495a7b`，搜索关键词高亮默认 `#3482d555`，后者的透明度位于最后两位。完整范围和刷新行为见第 11.3 节。

### 8.1 被标记聊天的补充搜索与特殊搜索

筛选依据为频道 / 超级群原始 `restriction_reason[]` 中存在 `reason == "porn"`，不限定 `platform` 必须为 `ios`，也不依赖简介文字或是否禁止转发。`ChannelData::hasPornRestriction()` 独立保留这个判断，避免客户端过滤不适用平台的显示限制时丢失标记。

- **我的聊天**：增强设置中的“全局搜索包含被标记 porn 的频道和群组消息”默认开启；在原有 `messages.searchGlobal` 结果之外，对已加入的被标记聊天逐个调用 `messages.search`，合并补搜结果。
- **特殊搜索**：在“按下列条件搜索”菜单中位于“我的聊天”下方、“公开聊天”上方，仅运行上述被标记聊天的搜索队列。关闭 `search_include_porn` 后仍可使用；该模式不发送普通全局消息搜索，也不混入原生全局缓存或公开帖子结果。

两种入口复用同一套来源筛选、分页、重试和调度逻辑；支持普通关键词及 `#标签`，按完整消息身份去重并按消息时间从新到旧合并，保留结果选择和滚动锚点。普通关键词也可显示搜索范围菜单，“公开聊天”仍遵循原有标签条件；特殊搜索保留频道 / 群组类型筛选，不提供“私人聊天”类型。

候选来源来自主列表、可选归档列表及置顶对话；是否包含归档遵循搜索范围选项，其默认值来自 `search_main_and_archive`。必要时通过 `channels.getChannels` 补齐频道元数据。升级群组会同时检索关联的旧群历史，进度按当前频道 / 群组去重计数。

主要提交：`937731ae46`、`210db5e479`。核对来源：[补搜 API 与目录管理](../Telegram/SourceFiles/api/api_porn_search.cpp)、[原始频道标记读取](../Telegram/SourceFiles/data/data_session.cpp)、[频道状态](../Telegram/SourceFiles/data/data_channel.h)、[搜索入口与合并](../Telegram/SourceFiles/dialogs/dialogs_widget.cpp)、[搜索范围菜单](../Telegram/SourceFiles/dialogs/ui/chat_search_in.cpp)。新增源码同时接入 [Telegram 构建清单](../Telegram/CMakeLists.txt)，菜单、设置和进度文案覆盖基础语言及本地中英文资源。

### 8.2 并发、请求间隔、限流提示与日志

“我的聊天”的补搜和“特殊搜索”共用“补充搜索并发数”和“补充搜索请求间隔”，修改后动态作用于队列。默认并发为 `3`，范围 `1–32`；默认请求间隔为 `500 ms`，范围 `0–5000 ms`。设为 `0 ms` 可去掉固定启动间隔，仍保留并发上限和服务端等待约束；默认间隔下每秒最多启动约 2 个队列请求，因此仅把并发从 3 调到 20 不一定提速。

对话目录、频道元数据和逐聊天搜索请求共用补搜队列的名额，这些参数不代表整个客户端全部 API 的并发数。调度还会在精确消息总数已满足时省去额外的确认空页，无消息变化时不重复重排结果。

捕获 `FLOOD_WAIT_*` / `FLOOD_PREMIUM_WAIT_*` 后显示 toast，同一等待窗口去重，并写入 `log.txt`。错误日志包含 API 方法、请求 ID、错误类型 / 码、等待时间、并发和请求间隔；目录总数确认及目录失效也有日志。等待期间暂停队列并保留分页游标，到期自动续搜；普通全局搜索也遵循共享的服务端 flood 等待。

主要提交：`937731ae46`、`5c53fddd98`。核对来源：[补搜队列与错误处理](../Telegram/SourceFiles/api/api_porn_search.cpp)、[调度策略](../Telegram/SourceFiles/api/api_porn_search_policy.h)、[设置默认值](../Telegram/SourceFiles/core/enhanced_settings.cpp)、[参数输入与校验](../Telegram/SourceFiles/boxes/enhanced_options_box.cpp)。设置位置和完整配置键见第 11.3 节。

### 8.3 固定搜索来源、实时进度与耗时

当前单行摘要依次显示状态图标、耗时、来源进度和实时消息数；耗时位于图标右侧，不再显示“已加载 / 条消息”等长文案。“我的聊天”混合搜索示例：

```text
🔄 (40s)【30/109】30 + 10 = 40
✅ (1m20s)【109/109】60 + 19 = 79 [12]
```

消息数按“原生已显示结果 + 补搜新增结果 = 当前合并总数”拆分，在搜索过程中持续更新，不是服务端预计总数。两路重复命中按完整消息 ID 只计一次并归入原生；原生页晚到时可调整归属而不增加合计，删除或过滤结果也同步更新计数。

“特殊搜索”只显示自身的消息总数，例如 `🔄 (40s)【30/109】40`；完成时改为 `✅`。末尾可选的 `[12]` 是当前定位结果从 1 开始的序号，导航行为见第 8.5 节。

分子是已完成首次检查的频道 / 群组数，不是 API 请求数或所有分页均已耗尽的来源数。初次目录尚未准备完可显示 `【0/…】`；补搜启动前先确认候选集合，之后本轮来源和分母固定。后台 `getDifference()` 或新频道信息导致共享目录失效时，已确认查询保持原来的总数、消息、失败状态和分页游标，不再从 `109` 退回 `…`，也不重复驱动目录扫描；下一次查询才使用更新后的目录。

首次目录 / 元数据加载失败时保留重试入口，不将不完整目录标为已确认。搜索耗时包含请求、排队和服务端等待，活动期间逐秒刷新；分页之间仅阅读结果的空闲阶段暂停计时，继续分页后累计，全部完成后冻结。混合搜索只有原生与补搜分页均耗尽、无失败、等待或在途加载时才显示 `✅`；特殊搜索按补搜队列的完成状态判断。其余状态使用 `🔄`，失败和等待仍有各自提示。

主要提交：`5c53fddd98`、`d19fb337a3`、`0812c5912b`、`7e9611749c`、`86c27d52bd`。核对来源：[来源快照、计时与原生计数策略](../Telegram/SourceFiles/api/api_porn_search_policy.h)、[查询状态更新](../Telegram/SourceFiles/api/api_porn_search.cpp)、[结果状态与计时刷新](../Telegram/SourceFiles/dialogs/dialogs_widget.cpp)、[进度绘制](../Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp)、[中文摘要文案](../Telegram/Resources/langs/localization/zh-CN.json)。早期进度富文本的参数类型由 `d8a908d85d` 修正；本轮短文案同步更新基础语言及本地中英文资源。

### 8.4 黄色圆形 18+ 标记与小锁对齐

搜索结果头像 / 图片左上角显示被标记频道和群组的黄色圆形 `18+`，左下角保留禁止转发小锁；两者共用 `no_forwards_badge_color`，默认黄色 `#ecbb71`，标记可同时显示。

以锁资源的可见图案边界计算圆形尺寸和横向中心，统一使用同一绘制矩形；去除锁资源内部 padding 对定位的干扰，同时保留锁原有的可见尺寸和位置，并修复从右向左界面的重复镜像。尺寸、图标偏移、内容边界和字体通过样式定义参与界面缩放。

主要提交：`937731ae46`、`5c53fddd98`、`b1890d1a2a`。核对来源：[标记绘制](../Telegram/SourceFiles/dialogs/ui/dialogs_layout.cpp)、[锁可见边界与偏移](../Telegram/SourceFiles/dialogs/ui/dialogs_layout.style)、[圆形与字体样式](../Telegram/SourceFiles/dialogs/dialogs.style)。已静态核对 1× / 2× / 3× 锁资源的可见边界；客户端外观仍需覆盖不同缩放、主题、RTL、头像与图片结果，以及两个标记同时出现的情况。

### 8.5 固定搜索进度与结果导航

- “我的聊天”补搜和“特殊搜索”的摘要滚动到结果列表顶部后保持可见，继续显示实时计数、耗时和聊天类型筛选；鼠标命中与键盘滚动为固定摘要预留位置，避免选中其下方被遮挡的结果。
- 摘要末尾的 `[序号]` 跟随鼠标打开、双击或键盘导航的当前结果，也随活动消息变化刷新。分页插入、重排或删除后按完整消息 ID 重新定位，结果消失或清空查询时移除序号。
- 消息搜索列表增加跳到“当前已加载结果底部”的按钮；有可滚动结果时按当前位置显示向上 / 向下按钮，到达底部后仍可直接回到顶部。后续分页沿用正常加载机制，按钮不表示一次获取所有剩余消息；窄列和子列表继续隐藏这些浮动按钮。

主要提交：`3f852526c1`。核对来源：[固定摘要与结果序号](../Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp)、[搜索滚动按钮](../Telegram/SourceFiles/dialogs/dialogs_widget.cpp)。固定摘要和序号仅用于上述补搜模式，滚动按钮也适用于普通消息搜索。

回归重点：搜索进行中滚动、筛选命中、键盘上下 / 翻页、打开或双击结果后的序号、异步补页与删除、无结果 / 全部完成、到顶 / 到底、窄窗口、主题与界面缩放。

## 9. 频道作为发送者标记、emoji/sticker 尺寸和消息展示

群组内频道作为发送者的消息可显示标记；可调整 custom emoji 和 sticker 尺寸并在重启后应用；同时保留消息 ID、时间秒数、媒体上传日期、文件/视频详情和相关展示增强。

关键入口：`Telegram/SourceFiles/history/view/history_view_message.cpp`、`Telegram/SourceFiles/history/view/media/history_view_custom_emoji.cpp`、`Telegram/SourceFiles/history/view/media/history_view_sticker.cpp`、`Telegram/SourceFiles/history/view/history_view_bottom_info.cpp`。

设置：`label_channel_user`（默认 `true`）、消息媒体尺寸 prefs、`show_messages_id`、`show_seconds`。代表提交：`dfb9223b89`、`ee4a9c81c1`、`6944977f76`、`77d84767e2`。

固定回归：普通用户、匿名管理员和频道 sender；单个/组合 emoji、贴纸、缩放和重启；消息 ID/秒数在普通、转发、服务和媒体消息中的布局。

## 10. 跳转、最近对话和自定义快捷键

增强设置支持常驻最近对话面板、跳转到对话、最多读取前 256 个非空配置项的自定义聊天快捷键、全局搜索快捷键，并提供搜索结果双击复制链接等快速操作。

关键入口：`Telegram/SourceFiles/core/shortcuts.cpp`、`Telegram/SourceFiles/core/shortcuts.h`、`Telegram/SourceFiles/window/window_session_controller.cpp`、`Telegram/SourceFiles/boxes/share_box.cpp`、`Telegram/SourceFiles/boxes/share_box.h`、`Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp`、`Telegram/SourceFiles/data/data_peer_id.h`。

设置：`chat_switch_persistent_shortcut`、`jump_to_dialog_shortcut`、`custom_chat_shortcuts`、`global_search_shortcut`。代表提交：`6db0088aad`、`ffd4862412`、`125fbe3c3f`。

固定回归：默认/空值/非法配置、修饰键释放、关闭面板后重开、256 个非空项和 4096 字符上限、聊天不存在、与系统/Compose 快捷键冲突。

“跳转到对话”默认 `Alt+E`，“全局搜索”默认 `Ctrl+Alt+F`，常驻最近对话和自定义对话快捷键默认留空停用。自定义项以首个英文逗号分隔目标与快捷键，多项以 `|` 分隔；支持数字 ID、带或不带 `@` 的用户名，以及 `t.me`、`tg://`、`internal:` 链接。保存后重新加载绑定，清空后保存移除；格式示例见第 11.3 节。

### 10.1 聊天选择器的搜索匹配与结果刷新

调整共用的 `ShareBox` 聊天选择器，覆盖“跳转到对话”和使用同一组件的分享 / 转发入口。

- 本地搜索在规范化的聊天名称词中同时支持前缀和词内子串匹配；每个查询词都必须命中。全部为前缀匹配的聊天排在前面，包含词内命中的结果随后显示，两组内保留当前聊天索引顺序。
- 聊天名称变化、收到远端搜索结果或切换文件夹后，重新计算匹配与去重；名称更新合并到主线程刷新，结果未变化时不重排，仍存在的当前键盘导航项按聊天身份恢复。
- 远端结果只排除已在本次本地匹配结果中的聊天；已存在于聊天列表但未被本地名称匹配命中的远端结果也可显示。同一聊天共用显示与选择状态，避免两套结果重复维护。
- 查询变化时清理旧远端结果，收到回包时核对规范化的当前查询；快速改词、清空输入或使用带 `@` 的用户名查询时，过期结果不会回填到新查询。

主要提交：`efa2a4c9c8`。核对来源：[聊天选择器匹配、刷新与入口](../Telegram/SourceFiles/boxes/share_box.cpp)。本轮不新增设置键，使用现有聊天索引和搜索入口。

回归重点：中文词内搜索、多关键词、前缀优先顺序、名称更新、切换文件夹、远端与本地重复结果、已有但本地未匹配的聊天、快速改词 / 清空 / `@` 查询、已选聊天与键盘导航项保持。

## 11. 增强设置、语言资源和旧配置兼容

`Core::EnhancedSettings` 集中保存本地选项，`Settings::Enhanced` 提供 UI；本地 `zh-CN.json` 优先加载并覆盖本地新增文案。新增设置应优先 KV prefs；顺序二进制序列化字段只能追加并使用 `!stream.atEnd()` 兼容旧 portable。

关键入口：`Telegram/SourceFiles/core/enhanced_settings.cpp`、`Telegram/SourceFiles/settings/settings_enhanced.cpp`、`Telegram/SourceFiles/boxes/enhanced_options_box.cpp`、`Telegram/Resources/langs/lang.strings`、`Telegram/Resources/langs/localization/zh-CN.json`、`Telegram/SourceFiles/storage/localstorage.cpp`。

代表提交：`6f294b71a5`、`dfda001c0a`、`ba3c7dfb5d`、`9d5b1fe33e`。固定回归：旧配置启动、默认值、设置保存/导出/导入、语言切换、未翻译 key fallback、重命名设置的迁移。

以下迁入原文的完整增强设置清单，包含继承自 64Gram 的选项；是否属于本机新增功能仍以历史提交归属为准。此处按页面列项，Google 翻译后端及格式保护另见第 3 节。

入口：`设置 → 增强设置`。页面按 `网络 → 消息 → 按钮 → 语音聊天 → 其他` 排列。以下以当前界面创建代码和实际调用处为准；默认值采用源码初始值，已有配置可能不同。消息分组中的右键菜单和 MPV 配置分别展开列出。

### 11.1 网络

| 功能 | 配置键 | 默认值 | 功能说明与生效方式 |
| --- | --- | --- | --- |
| 上传速度提升 | `net_speed_boost` | 已禁用（0） | 可选已禁用、轻度、中等、高（0–3），调整上传并发、会话数和请求间隔；保存时确认重启。 |
| 优化在线播放 | `net_download_speed_boost` | 已禁用（0） | 可选已禁用、轻度、中等、高、激进、极限、智能（0–6），调整下载、预读、拖动进度后的恢复及 MPV 缓存策略；智能档根据吞吐、缓冲和限速反馈自适应调整。保存时确认重启。 |
| 内置播放器默认播放原画质 | `video_player_prefer_original` | 开启 | 打开视频时优先选择原文件，并按固定原画开始播放；播放中仍可手动切换画质。保存后下次打开视频生效，无需重启，可能增加流量消耗。详见第 1.2 节。 |
| 优化在线播放参数配置 | `net_download_speed_boost_profiles` | 各档位的内置参数 | 分别编辑七个档位，提供还原当前档位、保存和取消；全部参数见第 11.2 节。 |
| 将在线播放调试日志写入 log.txt | `online_playback_debug_logs` | 关闭 | 记录加载、预读、拖动及调度信息，并采集内置播放器与 MPV 的播放、传输诊断统计；支持即时切换，完整采样应先开启再打开视频。详见第 1.7 节。 |
| 非 Premium 限速等待覆写 | `flood_premium_wait_override_ms` | 留空，采用服务端等待时间 | 填写非负整数毫秒值，覆盖 `FLOOD_PREMIUM_WAIT_x` 的客户端重试等待；0 表示不额外等待。虽然入口标红，当前处理请求时直接读取配置，新遇到的限速事件可使用新值。 |

在线播放优化和等待覆写调整的是客户端请求策略；实际速度仍受网络、数据中心及服务端限速影响。

### 11.2 优化在线播放参数配置

每个分片为 128 KiB。弹窗中的档位选择用于编辑对应参数，实际启用哪个档位仍由上一项“优化在线播放”决定。切换编辑档位时先校验并暂存当前值；保存会写入全部档位，重启后应用。“还原当前档位”恢复当前编辑档位的内置值，随后仍需保存。

以下覆盖全部 22 个数值项及 2 个开关，数值默认值随档位而定：

| 分组 | 参数 | 字段 | 允许范围 | 作用 |
| --- | --- | --- | --- | --- |
| 播放与 Reader | 并发请求上限 | `requestsLimit` | 1–32 | 同时请求的视频分片数量上限。 |
| 播放与 Reader | 基础预读分片数 | `preloadPartsAhead` | 1–64 | 在播放位置前方提前加载的分片数。 |
| 播放与 Reader | 文件尾预读分片数 | `tailPrefetchParts` | 0–16 | 为 MP4 索引等尾部信息预读的分片数。 |
| 播放与 Reader | 拖动取消跳跃分片数 | `seekCancelJumpParts` | 1–256 | 拖动距离达到相应阈值时取消旧位置的请求。 |
| 播放与 Reader | 拖动取消保护分片数 | `seekCancelGuardParts` | 0–64 | 取消旧请求时保留播放点附近的在途分片。 |
| 播放与 Reader | 远程播放提前加载毫秒数 | `loadInAdvanceMs` | 0–300000 ms | 播放器期望提前准备的播放时长。 |
| 播放与 Reader | 远程播放等待缓冲毫秒数 | `waitingBufferMs` | 0–30000 ms | 短暂缺少数据时的缓冲等待参数。 |
| 播放与 Reader | 非 Premium 预读上限 | `nonPremiumPreloadLimit` | 1–128 | 从服务端限速恢复期间的预读分片上限。 |
| 播放与 Reader | 启用拖动旧请求取消 | `seekCancelEnabled` | 开 / 关 | 控制远距离拖动时是否取消旧位置的请求。 |
| 播放与 Reader | 启用文件尾预读 | 通过 `tailPrefetchParts` 保存 | 开 / 关 | 关闭时将文件尾预读分片数保存为 0；开启而数值为 0 时自动设为 1。 |
| 下载会话与 MPV | 下载会话初始等待分片数 | `startWaitedParts` | 1–128 | 单个下载会话启动时允许积压的分片数。 |
| 下载会话与 MPV | 下载会话最大等待分片数 | `maxWaitedParts` | 1–256 | 单个下载会话允许积压的分片上限。 |
| 下载会话与 MPV | 初始下载会话数 | `startSessions` | 1–32 | 开始下载时的并发会话数。 |
| 下载会话与 MPV | 最大下载会话数 | `maxSessions` | 1–64 | 下载管理器允许扩展到的会话上限。 |
| 下载会话与 MPV | MPV 文件尾预读分片数 | `mpvTailPrefetchParts` | 0–16 | MPV 播放时额外读取的文件尾范围。 |
| 下载会话与 MPV | MPV 最大缓存 MB | `mpvCacheMaxMb` | 0–4096 MB | MPV 解封装缓存上限。 |
| 下载会话与 MPV | MPV 回看缓存 MB | `mpvCacheBackMb` | 0–1024 MB | MPV 为向后回看保留的缓存。 |
| 智能参数 | 智能最小预读 | `smartMinimumPreload` | 1–64 | 限速时保留的最低预读分片数。 |
| 智能参数 | 智能最小并发 | `smartMinimumRequests` | 1–32 | 限速时保留的最低 Reader 并发。 |
| 智能参数 | 智能最大预读 | `smartMaximumPreload` | 1–64 | 智能档允许的最高预读分片数。 |
| 智能参数 | 智能 DC 初始并发 | `smartInitialRequestLimit` | 1–32 | 首次播放某个数据中心的媒体时使用的请求并发。 |
| 智能参数 | 智能 DC 最小并发 | `smartMinimumRequestLimit` | 1–32 | 服务端限速时可降到的最低并发。 |
| 智能参数 | 智能 DC 最大并发 | `smartMaximumRequestLimit` | 1–32 | 缓冲压力探测时可升到的最高并发。 |
| 智能参数 | 智能容量回落下限 | `smartCapacityMinimumRequestLimit` | 1–32 | 带宽充足时自动降低并发的停止值。 |

保存时同时校验：最大等待分片数不小于初始值，最大会话数不小于初始值，回看缓存不大于最大缓存，智能最大预读不小于最小预读，智能 DC 初始并发和容量回落下限均处于 DC 最小、最大并发之间。智能参数仅对智能档生效，其他档位仍可保存这些值。

### 11.3 消息

| 功能 | 配置键 | 默认值 | 功能说明与生效方式 |
| --- | --- | --- | --- |
| 显示消息 ID | `show_messages_id` | 关闭 | 在消息底部时间信息中附加消息 ID；切换后自动重启。 |
| 标注频道身份发言 | `label_channel_user` | 开启 | 为以广播频道身份发出的群消息显示“频道”标记。 |
| 表情和贴纸尺寸 | `message_emoji_size`、`message_sticker_size` | 112 px / 256 px | 分别调整动画表情和贴纸尺寸，范围为 50–112 px、50–256 px；支持恢复默认尺寸，修改后确认重启。 |
| 显示复读机选项 | `show_repeater_option` | 关闭 | 启用消息复读功能，菜单可提供复读为转发、复读为复制；还需开启第 11.4 节的“复读机”菜单开关。 |
| 回复原始消息 | `repeater_reply_to_orig_msg` | 关闭 | 复读时回复原消息。仅在“显示复读机选项”已开启时显示此设置；开启后可重新进入页面查看。 |
| 始终默认删除对象 | `always_delete_for` | 已禁用（0） | 可选已禁用、群组、个人、两者都选（0–3），控制对应删除确认框的默认勾选状态。 |
| 停止从云端同步草稿 | `disable_cloud_draft_sync` | 关闭 | 忽略收到的云端草稿及草稿清空更新，避免覆盖当前草稿。 |
| 隐藏经典转发 | `hide_classic_fwd` | 关闭 | 隐藏经典转发相关的右键菜单和多选顶部栏入口。 |
| 跨对话保留选中的消息 | `keep_selected_messages_across_chats` | 关闭 | 切换对话时保留选中状态；关闭此项会清空跨对话保存的选中消息。 |
| 快速复制到... | `quick_copy_targets` | `-1002615379741,Saved Messages` | 设置多选顶部栏“一键复制”的目标对话列表；留空停用，目标解析和删除原消息行为见下文。 |
| 自定义对话快捷键 | `custom_chat_shortcuts` | 留空，已禁用 | 绑定快捷键到指定对话或 Telegram 链接；保存后重新加载，格式见下文。 |
| 双击搜索结果复制消息链接 | `double_click_copy_link` | 关闭 | 在搜索结果列表中双击消息结果，将对应消息链接复制到剪贴板。 |
| 禁止转发徽标颜色 | `no_forwards_badge_color` | `#ecbb71` | 修改禁止转发小锁颜色，同时控制被标记聊天的 `18+` 圆形底色；可填写 `#RRGGBB`，留空恢复默认。 |
| 代码块背景色 | `code_block_bg_color` | `#495a7b` | 修改消息代码块背景色；可填写 `#RRGGBB`，留空恢复默认，保存后通知界面刷新配色。 |
| 搜索跳转消息关键词高亮背景色 | `search_message_highlight_bg_color` | `#3482d555` | 设置从搜索结果跳转到消息后的关键词背景色，支持 `#RRGGBB` 和 `#RRGGBBAA`，透明度位于最后两位；留空恢复默认。 |
| 禁用打开链接警告 | `disable_link_warning` | 关闭 | 跳过受该选项控制的链接打开警告确认。 |
| 禁用 Premium 动画 | `disable_premium_animation` | 关闭 | 禁用 Premium 动态头像及 Premium 贴纸附加特效。 |
| 禁用全局搜索 | `disable_global_search` | 关闭 | 停止向服务端进行全局用户、群组和频道检索；已有对话和消息搜索仍可使用。 |
| 全局搜索同时包含主列表和已归档对话 | `search_main_and_archive` | 开启 | 全局搜索默认同时覆盖主列表与归档对话。 |
| 全局搜索包含被标记 porn 的频道和群组消息 | `search_include_porn` | 开启 | 在“我的聊天”中合并被标记聊天的逐聊天补搜结果；旧配置缺失该键时同样默认开启。此开关不影响独立“特殊搜索”。 |
| 补充搜索并发数 | `search_porn_concurrency` | 3 | 范围 1–32；“我的聊天”补搜与“特殊搜索”共用，修改后动态作用于队列。 |
| 补充搜索请求间隔 | `search_porn_request_interval_ms` | 500 ms | 范围 0–5000 ms；两种入口共用，修改后动态作用于队列。0 ms 去掉固定启动间隔，仍保留并发上限与服务端等待约束。 |
| 在群组中显示发送者头像 | `show_group_sender_avatar` | 关闭 | 在对话列表的非话题群组头像角标中显示最后一条消息发送者的头像。 |
| 使用正體中文翻譯訊息 | `translate_to_tc` | 关闭 | 将 Google 翻译的目标语言设为繁体中文 `zh-TW`；仅基础语言包为 `zh-hant-raw` 或 `zh-hans-raw` 时显示此开关。 |
| 时间显示秒数 | `show_seconds` | 关闭 | 消息时间及相关时间显示包含秒数；切换后约 1 秒自动重启。 |
| 显示“查看为 JSON” | `show_json` | 关闭 | 在消息菜单中提供“查看为 JSON”，查看消息的结构化数据。 |
| 隐藏被拉黑用户的消息 | `blocked_user_spoiler_mode` | 关闭 | 将被拉黑用户的消息文字转为剧透格式，并阻止媒体自动下载，消息仍可查看。需重启以完整应用；启用时若拉取并保存黑名单，会在保存后约 3 秒自动重启。 |

“特殊搜索”是搜索范围菜单入口，不另设启用配置；上述并发与间隔参数在关闭 `search_include_porn` 时仍供它使用。搜索范围、结果合并和进度语义见第 8 节。

“始终默认删除对象”中的“群组”会在适用的群组管理删除弹窗中默认勾选删除消息和反应；“个人”会默认勾选适用弹窗中的“也为对方删除 / 为所有人删除”；“两者都选”同时应用。实际删除仍需确认并受当前权限约束。

“快速复制到...”使用英文逗号分隔多个目标，支持用户 ID、普通群负 ID、频道 / 超级群 `-100...` ID，以及不区分大小写的 `Saved Messages`。除“已保存的消息”外，目标需要已被当前会话加载；重复目标会去重，无效或不可发送目标会被跳过。默认字符串表示两个目标。

**一键复制当前还会尝试删除原消息**：全部目标转发成功、没有目标被跳过且所选内容不含已保存音乐时，对可删除的原消息请求删除，并带有为所有人删除的意图；结果仍取决于服务端权限。该删除步骤没有独立的增强设置开关，也不会再次弹出删除确认框。

“自定义对话快捷键”格式示例：

```text
@username,alt+a|https://t.me/durov,alt+d|-1001732671681,alt+f
```

每项以首个英文逗号分隔目标和快捷键，多项以 `|` 分隔。目标支持数字对话 ID、带或不带 `@` 的用户名，以及应用可解析的 `t.me`、`tg://`、`internal:` 链接。最多读取前 256 个非空项，输入框最多 4096 个字符；清空后保存可移除这些自定义绑定。

### 11.4 消息右键菜单逐项开关

以下 21 项都位于“消息”分组，源码默认全部开启。开关控制入口是否显示，实际菜单仍受消息类型、发送状态、操作权限及相关功能开关影响。

| 菜单项 | 配置键 | 功能说明 |
| --- | --- | --- |
| 已读 / 反应信息 | `show_message_context_read_info` | 显示适用消息的已读和反应信息。 |
| 详情 | `show_message_context_details` | 查看消息 ID、时间、浏览量、转发数，以及文件大小、格式、分辨率、编码等适用信息。 |
| 回复 / Reply | `show_message_context_reply` | 回复当前消息。 |
| 添加任务 / Add Tasks | `show_message_context_add_task` | 在适用的任务列表消息中添加任务。 |
| 复制消息链接 / Copy Message Link | `show_message_context_copy_link` | 复制当前消息或频道帖子的链接。 |
| MPV 播放（特殊） | `show_message_context_stream_in_mpv_special` | 仅 Windows；按住 `Ctrl` 右键消息时显示，使用特殊视频兼容播放路径。 |
| mpv 播放 | `show_message_context_stream_in_mpv` | 仅 Windows；调用外部 MPV 播放适用媒体。 |
| 查看该用户消息 | `show_message_context_show_messages_from` | 在当前对话中查找该发送者的消息。 |
| 转发 | `show_message_context_forward` | 显示消息转发入口及适用的转发子菜单。 |
| 复读机 | `show_message_context_repeater` | 显示复读菜单，还需开启“显示复读机选项”。 |
| 立即发送 / Send Now | `show_message_context_send_now` | 立即发送定时消息。 |
| 转到消息 / Go To Message | `show_message_context_go_to_message` | 跳回原消息所在位置。 |
| 查看回复 / View Thread | `show_message_context_view_replies` | 打开消息的回复线程或讨论。 |
| 编辑 / Edit | `show_message_context_edit` | 编辑允许修改的消息。 |
| 添加事实核查 / Add Fact Check | `show_message_context_factcheck` | 显示有权限时可用的事实核查操作。 |
| 置顶 / Pin | `show_message_context_pin` | 显示适用的置顶、取消置顶操作。 |
| 删除 / Delete | `show_message_context_delete` | 删除当前或已选消息。 |
| 另存为 / Save As... | `show_message_context_save_as` | 将适用的消息媒体保存到文件。 |
| 举报 / Report | `show_message_context_report` | 举报当前消息。 |
| 选择 / Select | `show_message_context_select` | 将消息加入多选。 |
| 重新安排 / Reschedule | `show_message_context_reschedule` | 调整定时消息的发送时间。 |

### 11.5 消息分组中的 MPV 配置

这两项与上述两个 MPV 菜单开关均仅在 Windows 构建中显示。

| 功能 | 配置键 | 默认值 | 功能说明 |
| --- | --- | --- | --- |
| 将 MPV 调试日志写入 log.txt | `mpv_streaming_debug_logs` | 关闭 | 为普通和特殊 MPV 播放路径记录详细日志，同时启用第 1.7 节的统一播放诊断统计；支持即时切换。 |
| mpv 路径 | `mpv_path` | 留空，使用 PATH | 指定 `mpv.exe` 路径；留空时查找系统 `PATH` 中的 MPV。 |

### 11.6 按钮

| 功能 | 配置键 | 默认值 | 功能说明与生效方式 |
| --- | --- | --- | --- |
| 将表情按钮显示为文字 | `show_emoji_button_as_text` | 关闭 | 把多选消息顶部栏中用 emoji 表示的转发、删除、无引用转发、保存等操作按钮改为文字；切换后自动重启。 |
| 始终显示定时发送按钮 | `show_scheduled_button` | 关闭 | 即使没有待发送的定时消息，也显示定时消息入口按钮。 |

### 11.7 语音聊天

| 功能 | 配置键 | 默认值 | 功能说明与生效方式 |
| --- | --- | --- | --- |
| 电台控制器 | `radio_controller` | `http://localhost:2468` | 配置实验性音乐流控制服务地址，用于通知语音聊天成员加入、离开等变化；留空保存会恢复默认地址。 |
| 自动取消静音 | `auto_unmute` | 关闭 | 加入语音聊天后，在权限允许时自动取消自身静音。 |
| 音频码率 | `bitrate` | 默认（32 Kbps，0） | 可选默认、64、96、128、160、192、256、320 Kbps（0–7）；已加入语音聊天时需重新加入。 |
| 更高视频码率 | `hd_video` | 关闭 | 启用语音聊天中的更高视频码率；已加入时需重新加入。 |

### 11.8 其他

| 功能 | 配置键 | 默认值 | 功能说明与生效方式 |
| --- | --- | --- | --- |
| 常驻最近对话面板快捷键 | `chat_switch_persistent_shortcut` | 留空，已禁用 | 自定义打开常驻最近对话面板的快捷键，例如 `Ctrl+Alt+P`；留空停用。 |
| 跳转到对话快捷键 | `jump_to_dialog_shortcut` | `Alt+E` | 打开“跳转到”对话选择面板；支持自定义，留空停用。 |
| 全局搜索快捷键 | `global_search_shortcut` | `Ctrl+Alt+F` | 触发应用内全局搜索；支持自定义，留空停用。 |
| 隐藏“所有聊天”文件夹 | `hide_all_chats` | 关闭 | 隐藏文件夹栏中的“所有聊天”入口；切换后自动重启。 |
| 用“已保存的消息”替换“编辑”按钮 | `replace_edit_button` | 关闭 | 将文件夹侧栏的编辑按钮替换为“已保存的消息”，切换后重新加载侧栏。 |
| 跳转到下一个未读聊天 | `skip_to_next` | 关闭 | 使用 `Alt+↑/↓` 切换对话时跳过已读项，按方向定位未读消息或手动标为未读的对话。 |
| 隐藏在线用户计数 | `hide_counter` | 关闭 | 隐藏超级群组标题栏的在线人数，仍显示成员总数。 |
| 隐藏动态 | `hide_stories` | 关闭 | 隐藏对话列表中的动态栏及相关动态入口。 |
| 降低聊天预览图亮度 | `preview_brightness_enabled` | 关闭 | 调暗聊天消息中的静态图片、GIF / 视频预览缩略图，包含相册预览；不改变实际播放和媒体查看器的亮度。 |
| 亮度等级 | `preview_brightness` | 70% | 仅在开启预览图亮度调节后显示；可选 10%–100%，步长 10%，100% 为原始亮度。 |
| 增强媒体查看器滚轮控制 | `media_viewer_wheel_control_enabled` | 关闭 | 将查看器横向分为三等分：左上滚轮调亮度、左下调音量，中间切换媒体，右侧调整视频进度。 |

### 11.9 核对来源

- [设置主页入口](../Telegram/SourceFiles/settings/sections/settings_main.cpp)：确认 `设置 → 增强设置` 的入口。
- [增强设置页面](../Telegram/SourceFiles/settings/settings_enhanced.cpp)：五个分组、全部设置项及条件显示逻辑。
- [配置弹窗](../Telegram/SourceFiles/boxes/enhanced_options_box.cpp)：可选档位、数值范围、输入格式、保存和重启行为。
- [增强设置默认值与保存](../Telegram/SourceFiles/core/enhanced_settings.cpp)、[表情和贴纸尺寸常量](../Telegram/SourceFiles/core/enhanced_settings.h)：配置键、默认值和持久化。
- [在线播放档位参数](../Telegram/SourceFiles/media/streaming/media_streaming_boost.cpp)：七个档位的内置参数与读取逻辑。
- [本地简体中文文案](../Telegram/Resources/langs/localization/zh-CN.json)、[基础语言文案](../Telegram/Resources/langs/lang.strings)：界面名称和说明。
- [一键复制实现](../Telegram/SourceFiles/history/history_widget.cpp)、[转发请求回调](../Telegram/SourceFiles/apiwrap.cpp)：目标解析、成功回调和删除原消息条件。
- [自定义快捷键加载](../Telegram/SourceFiles/core/shortcuts.cpp)、[快捷键目标打开](../Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp)：配置项数量限制及目标格式。
- [初始视频画质选择](../Telegram/SourceFiles/data/data_document.cpp)、[媒体查看器](../Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp)：默认原画、手动画质切换与自动降画质请求的处理。
- [播放诊断统计](../Telegram/SourceFiles/media/streaming/media_streaming_diagnostics.cpp)、[日志开关判定](../Telegram/SourceFiles/media/streaming/media_streaming_debug.h)：两个日志开关的共同采集范围与即时切换。

补充本地化入口：`Telegram/Resources/langs/localization/en.json`、`Telegram/Resources/qrc/telegram/telegram.qrc`、`Telegram/SourceFiles/settings/settings_experimental.cpp`。历史中还通过 `b8e7719f0d` 修正语言资源加载、`6b183659f7` 微调英文文案；本地中文覆盖范围包括增强设置、MPV、消息详情、在线播放、跳转、亮度/音量和新增镜像菜单。

### 11.4 触控板惯性滚动与旧实验设置兼容

Qt 6 的实验设置 Interface（界面）分组显示“触控板惯性滚动”，保存键为 `kinetic-scroller`；Windows/macOS 默认关闭，其他平台默认开启。Qt 5 不注册或显示此选项。

启动与实验设置导入均兼容旧 `qscroller` 布尔值：只有新键不存在且旧值是布尔类型时迁移，新键显式存在时始终优先。启动先应用兼容值，再初始化实验设置持久化路径，避免在 Qt 应用创建前启动写盘计时器；常规保存和导出沿用注册选项的序列化规则，使用新键。新键类型错误时导入按原有类型校验失败，不回退到旧值。

入口：[实验设置兼容处理](../Telegram/SourceFiles/core/experimental_options.cpp)、[启动入口](../Telegram/SourceFiles/core/launcher.cpp)、[实验设置界面](../Telegram/SourceFiles/settings/settings_experimental.cpp)、`Telegram/lib_ui/ui/widgets/kinetic_scroller.*`。中英文资源键为 `lng_settings_experimental_kinetic_scroller` 和 `_desc`。这是本轮尚未提交的 F08 适配，不修改 app/session 的顺序二进制存储。

回归重点：旧/新键各自 true/false、二者冲突、新键错误类型、Qt 5/6、旧 portable 启动、导入/导出和触控板释放后的惯性滚动。当前完成静态分支与初始化顺序核对，客户端运行回归待完成。

## 12. 构建、CI、PCH、子模块与云验证

本地维护 Windows x64 Actions workflow、runner/VS/Breakpad 兼容、sccache 和 cache；历史上还调整过 PCH、生成顺序、Qt/FFmpeg/prepare.py 及多个子模块来源。当前主要云构建入口是 `.github/workflows/build-win-x64.yml` 的 `Build Windows x64`。

关键入口：`.github/workflows/build-win-x64.yml`、`.github/workflows/win.yml`、`Telegram/CMakeLists.txt`、`Telegram/build/prepare/prepare.py`、`.gitmodules` 与各 gitlink。

代表提交：`5942dee0ee`、`faa1314c05`、`234c4c044e`、`3bf9253d3f`、`3f93e09a31`、`5cfa11bdd5`、`335f521d65`、`e435e1b2ae`、`19d3040932`。

固定回归：workflow push 触发不重复、Secrets 只检查存在不泄露、runner/VS、cache key、sccache 长链接、PCH 独立 include、生成语言顺序、主仓库 API 与子模块 gitlink/worktree 一致。无真实 Actions 结果时不能写“编译通过”。

### 12.1 当前 Windows x64 产物与回归目标

- GitHub Actions 从 Debug 产物恢复为 Release 产物：上传名称为 `Telegram-x64`，文件为 `out/Release/Telegram.exe`；第三方库准备、缓存键和更新验证器路径同步调整。
- MP4 文件头与索引、读取停滞恢复、拖动预取、启动缓冲、缓存、MPV 可取消读取与索引重载共 8 个测试目标接入 Windows x64 工作流；其中索引解析与 IPC 重载目标由本轮提交新增。
- 测试目标构建失败、产物缺失或执行失败时，工作流退出；继续扩充 `smart_policy_check.py` 对智能档策略的静态检查。
- 被标记聊天的搜索回归通过独立步骤 `Check supplemental search progress, merging and limits` 接入完整构建之前，使用 MSVC `cl /std:c++20 /EHsc /W4 /WX` 编译并运行 `test_porn_search.cpp`，直接验证生产搜索策略。

主要提交：`d65d20e558`、`89a2be9e45`、`70f630ea15`、`90e017416d`、`aad1c1f633`、`666741cddb`、`797ad28e62`、`db21ec8d53`、`a84a5a0c9c`。

核对来源：[Windows x64 工作流](../.github/workflows/build-win-x64.yml)、[测试目标定义](../Telegram/cmake/tests.cmake)、[智能档静态检查](../Telegram/SourceFiles/media/streaming/tests/smart_policy_check.py)、[搜索回归](../Telegram/SourceFiles/test/test_porn_search.cpp)、[被测试的搜索策略](../Telegram/SourceFiles/api/api_porn_search_policy.h)。2026-09-11 的文档整理只记录既有流媒体构建与检查配置，未执行构建、运行测试或查询对应 Actions 结果。

上述八个目标为 `test_mp4_header`、`test_mp4_index`、`test_mpv_index_reload`、`test_streaming_read_stall`、`test_streaming_seek`、`test_streaming_startup`、`test_streaming_cache`、`test_streaming_mpv`。新目标同时接入 Telegram 构建依赖，Windows 工作流中的元数据检查步骤改名为 `Check streaming startup, cache, MPV and MP4 metadata`。这是仓库已有云构建配置的记录，本次未触发构建；本地调试仍遵守仓库的 Debug 构建规则。

[MP4 索引测试](../Telegram/SourceFiles/test/test_mp4_index.cpp) 覆盖分片布局、时间偏移、原始字节偏移、读取预算、缓存探测与取消；[MPV 重载测试](../Telegram/SourceFiles/test/test_mpv_index_reload.cpp) 覆盖目标通知缺失 / 合并、晚到查询、重复目标、新目标接管、回跳等待、暂停、EOF 和停止。另新增 [真实 MPV 布局复现脚本](../Telegram/SourceFiles/test/test_mpv_seek.py)，用本地 HTTP 服务和可配置冷块延迟检查首播、跳转与音视频对齐；该脚本未作为上述 Actions 步骤运行，也不能单独代替生产 IPC 控制器回归。

[MPV 专项文档](mpv-fragmented-seek-fix.md) 保留独立 MSVC Debug 测试及本地真实 MPV 对照的历史记录。本次未重跑这些用例；独立驱动、替身 Reader 和模拟延迟结果不代表完整 Telegram 客户端或真实 MTProto 网络播放已经验证。

搜索回归由 `937731ae46` 引入，经 `5c53fddd98`、`d19fb337a3` 扩充，覆盖消息合并、时间排序、完整消息 ID 去重、乱序 / 重复分页、游标与精确 count；也覆盖 150 个聊天、并发 1 / 3 / 10 / 20 / 32、0 / 500 ms 请求间隔、flood 等待与提示去重、耗时暂停 / 恢复，以及确认 109 个来源后目录失效 / 恢复仍保持查询快照、新查询获取新目录和空目录确认。

`0812c5912b` 进一步覆盖原生 / 补搜计数拆分、两路重复命中、移除或过滤结果、原生页晚到时的归属调整、空结果与特殊搜索零原生贡献。

2026-09-12 云验证记录：`210db5e479` 的 [Windows x64 Actions](https://github.com/ked33/tdesktop/actions/runs/34672886083) 中，搜索回归步骤已通过；补录查询时完整 `Build Telegram (Release)` 仍在执行。该记录不代表客户端运行和界面外观已经验收，本次文档补录未启动新构建。

`3ce106af1f` 另修复 v7.2.5 合并后的构建接口适配并保存已准备的依赖缓存。历史中的 Node.js 20 Actions 弃用提示、workflow 重复触发和缓存未命中修正继续保留在附录。

### 12.2 CircleCI 和其他平台工作流的历史边界

`.circleci/config.yml` 仍在仓库中，源记录的四次提交属于 Windows x64 构建尝试，覆盖 checkout、工作目录、工具链探针与 artifact 路径；本次未验证其可用性。非 Windows 的 `linux.yml`、`mac.yml`、`mac_packaged.yml`、`snap.yml`、`docker.yml`、`changelog.yml` 曾经删减或调整，不能把那次删减当作当前文件状态。相关时间线和辅助文件清理见历史附录。

### 12.3 v7.2.8 配套依赖与静态检查

本轮完整采用 tlottie，移除 rlottie 的 gitlink、CMake 注册和链接；配方固定 Rust 1.96.1、tlottie `4b940c7942`、Qt 6.11.2，Qt 5 保持 5.15.19。六个被上游更新的子模块采用目标 SHA，`tgcalls` 和 `lib_storage` 的本地定制指针及 fork URL 保留。39 个递归子模块已核对 HEAD/gitlink、内部清洁和无 alternates。

新增源码同时检查 Telegram 可执行目标及 `td_ui` 目标的注册，资源、style using 闭包、直接生成头和依赖 API 一起审查。History 高亮与媒体帧填充补充直接 `style_basic.h` include，以明确所用样式常量的来源。现有 Smart 策略检查通过；详细范围、工具证据和静态检查的限制见 [本轮验证](upstream-merges/v7.2.8/verification.md)。本机未编译或运行客户端，本轮尚未触发新的 Actions。

## 13. 产品身份、版本与 64Gram 继承功能

该分支长期保留 64Gram 产品身份、资源、增强设置和大量早期 fork 功能，例如 Chat ID、管理员标记、转发/重复、隐私/故事/文件夹/群组通话选项等。`features.md` 是早期 64Gram 功能目录，但不包含 2026 年新增功能，不能替代本文件。

合并版本资源时默认保留本地 `AppId`、`AppName`、`AppFile`、公司/产品名和公开产品版本，只更新 official `UpstreamVersion`；任何策略变化写入当次冲突决策。必须逐项核对 `core/version.h`、`Telegram/build/version`、RC、UWP、`setup.iss`、desktop/metainfo/service 和 changelog，禁止整文件 ours/theirs。

本轮候选保持 `AppVersion=7000009`、公开版本 `7.0.9` 和 64Gram 身份，只将 `UpstreamVersion` 更新为 `7.2.8`。默认更新打包继续使用 v1，Windows x86/x64 安装包仍为 `64Gram-setup[-x64].<版本>.exe`；build/deploy/release 的制品名称一致。只有显式 `TDESKTOP_UPDATE_V2=1` 才进入 v2 命名、最低版本和官方签名检查，当前公开版本不满足 v2 的 7.2 最低版本要求。生产更新验签与信任边界未改动，实际安装、签名和更新服务仍需单独验证。

## 14. 当前已知维护债务

- 历史附录保留原有保守分类，并补充已确认的 AVIO 入口替代、CI 实验和仓库清理说明；`active` 不等于每一行都经过独立运行时验证，其他状态仍需随代码审计修订。
- 根目录 `我的修改.md` 本次保持原样，已把旧历史、完整设置和截至 2026-09-11 的功能更新迁入本组文档；后续以正文及历史附录维护，避免两处继续分叉。旧 `--all` 范围中有 5 条非当前 HEAD 可达提交，见附录补遗，不计入当前 330 条主表。
- 本地没有可依赖的 Telegram C++ 构建环境；静态检查、Actions 编译和用户运行验证必须分开记录。
- 旧快照中的待合并 `v7.1.2` 描述已被后续进展替代：本地 HEAD 已包含 `e800800df3` 合并的官方 `v7.2.5`。本轮已在独立升级副本合入固定 v7.2.8 目标并完成 F01–F08 适配，原 checkout 仍保持原样；正式 HEAD/下一轮 anchor 要以之后实际创建的合并提交为准。
