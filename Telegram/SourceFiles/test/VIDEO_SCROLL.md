# 视频播放器开关时的聊天位移诊断

这是观察代码，不改变播放、重排、锚点保存或滚动行为。辅助代码位于
`Telegram/SourceFiles/test/test_video_scroll.cpp`，普通云构建也会包含，不需要
`-testagent` 或本地 Debug 构建。

## 使用

1. 安装包含此改动的 GitHub Actions 构建。
2. 在设置中搜索并开启“将在线播放调试日志写入 log.txt”。
3. 完全退出并重新启动客户端，打开原聊天中间位置的视频 A，观察位移后关闭。
4. 关闭后等待约 5 秒，再打开、关闭同一个 A；最后再等待约 5 秒。
5. 查看现有 `log.txt` 中以 `Video Scroll:` 开头的行。便携版通常位于程序目录。
   保留首轮异常和第二轮正常的完整记录，随后关闭上述日志开关。

不要求视频能够自动播放。捕获从手动激活视频点击处理器开始；`open-dispatch`
标记进入打开文档调用，随后记录播放器显示、共享播放器锁定、清理和关闭。
当前入口覆盖普通聊天的 `HistoryInner`，其他消息列表入口不在本次捕获范围内。

## 节流与范围

- 每次启动最多捕获 8 轮视频操作，`cycle` 区分各轮；`cyclesLeft=0` 后需要重启。
- 打开和关闭各观察 5 秒。打开窗口过期后停止采样，稍后关闭时重新观察 5 秒。
- 同类事件的相同状态在 250 毫秒内合并；状态变化不按重复事件丢弃。
- 每 250 毫秒最多写 24 条普通事件，每个阶段最多 96 条；边界和汇总另计。
- 汇总中的 `duplicates` 是合并数量，`limited` 是达到上限后省略的数量。
  限流时额外保留首条、末条省略事件及实际滚动位置范围，不把“没日志”当成“没发生”。
- 定时采样每 250 毫秒检查一次，仅状态变化时记录；同步观察点用于保留短暂位移。
- 不记录消息正文、联系人名称、媒体地址或凭证。会记录消息及文档数字 ID。
- 上述上限只约束 `Video Scroll:`；同一开关启用的已有播放日志沿用原有节流。

## 主要字段

| 字段或事件 | 含义 |
| --- | --- |
| `cycle` / `phase` / `t` | 操作编号、打开或关闭阶段、距本轮开始的毫秒数 |
| `scroll` / `max` / `over` | 实际滚动位置、最大位置、弹性越界距离 |
| `viewport` / `list` | 视口和列表的本地几何信息：x、y、宽、高 |
| `listGlobalY` / `screenY` | 列表原点、目标消息顶部的屏幕坐标 |
| `historyTop` / `top` | 聊天内容起点、目标消息顶部在列表中的位置 |
| `anchor` / `migrated` | 当前／迁移前历史的锚点消息 ID、偏移、历史高度 |
| `savedTop` | 锚点推导的滚动位置；无有效锚点时为 -1 |
| `message` / `media` / `optimal` | 当前消息尺寸、当前媒体尺寸、媒体最优尺寸 |
| `pending` / `viewPending` | 历史和目标消息是否等待重排 |
| `resize-request` / `causeMsg` | 请求重排的来源消息；不一定是点击的视频消息 |
| `inline-optimal` / `inline-current` | 本次媒体尺寸计算结果 `size`；`value` 是活跃内嵌流是否存在，`frame` 是用于计算的帧尺寸 |
| `inline-ready` / `inline-stop-*` | 内嵌流就绪和清理过程；并不表示用户开启了自动播放 |
| `geometry-enter` | `value` 为 `_topDelta`，`extra` 为滚动补偿类型 |
| `geometry-target` | `value` 为目标位置，`extra` 为本次滚动补偿量 |
| `scroll-request` / `scroll-applied` | 请求滚动与调用完成后的状态，`value` 为请求位置 |
| `paint-layout` | 绘制入口发现待重排消息，直接更新列表尺寸 |
| `anchor-skipped-pending` | 因待重排而跳过保存当前锚点 |

先对比 `screenY`、`scroll` 与 `listGlobalY`，再沿同一轮的事件顺序寻找首次变化。
观察点可能位于一次布局更新的中间，因此应连同后续 `list-size-after`、
`scroll-applied` 和阶段汇总一起判断，不能单凭一条中间状态认定根因。
