# v7.2.8 自动合并部分的最终审查

对象是 source_head `efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11` 与官方 `272f6f5c2d29d8cdb3aec15907d616b87451a3ca` 的已解决合并索引。以下为最终源码及相对 source_head 的 diff 审查，不把早期预演结果当作客户端验证。

## 保留与适配范围

同一 merge-base 下，354 个仅本地修改路径中只有四项计划内变化：F08 两份英中 JSON，以及两处直接 `style_basic.h` include。181 个仅上游修改路径中只有 F06 的 build.sh、deploy.sh、release.py、release.sh 四项适配。其他这些路径的 mode/blob/gitlink 与各自来源一致，见 [final-preservation.json](final-preservation.json)。

本地两条编辑导航、选择/拖选及快速复制的 10 组函数体（含重载）与冻结 source_head 一致，详见 [adaptation-checks.json](adaptation-checks.json)。这是函数体保留证据，不等于整体 UI 行为已经运行验证。

## History、Compose 与附件

- `HistoryWidget::confirmSendingFiles`、`ChatWidget::confirmSendingFiles`、`ScheduledWidget::confirmSendingFiles` 均接入 SingleFolderPath、FolderFilesForSending、PrepareFolderArchive/PrepareFilesArchive；三个入口的编辑状态判断和拖放归档回调配套存在。
- `SendGifWithCaption`、`SendGifWithCaptionBox` 的声明、实现、内部转发和两个产品调用方均采用第三个 `Ui::PreparedList&&` 参数。legacy HistoryWidget 回调由 `crl::guard(this, ...)` 保护，ComposeControls 回调由 `_field` 保护；编辑结果通过原 sendingFilesConfirmed/_sendAsFileConfirmed 路径发送。
- GIF 预备数据的异步任务持有 QByteArray 副本，返回 UI 时通过 widget weak guard；编辑器回调由 box/parent guard 保护。没有将短生命周期 widget 直接交给一个新增的 session-owned RPC 回调。
- `HistoryInner::paintEvent` 与 `ListWidget::paintUserpics` 的新 visualTranslationFor 位移和反向恢复对称；本地 toggleItemSelectionAsGroup/applyDragSelection 保持原函数体。
- `KeyboardTextSelection::extend` 已采用上游支持对象占位符的按词边界 API。字体整形采用目标 lib_ui；这部分仍需真实字体、emoji、选择/复制回归。

## 菜单与窗口

- TopBarWidget 新 `_menuButton` 使用 weak_qptr；销毁回调保留对 TopBar 自身的 weak guard。closeMenu/unrippleMenuButton 声明、实现和调用配套。
- `KeepHoveredWhileShown` 的事件过滤器绑定 menu lifetime；关闭菜单后的 hover/ripple 恢复路径已纳入上游实现。
- `CanShowSeparateWindow` 声明、实现和转发菜单调用闭合；新增窗口前的可用性/锁定判断与 `ensureSeparateWindowFor` 的断言配套。
- 系统外部媒体查看器采用上游保存限制及阅后即焚限制。本地 MPV 普通/特殊入口、帧复制和 no-forwards 定制没有被本轮这些修改替代；这些入口各自原有的策略边界仍需回归。

## 媒体、线程与网络

- `Media::Video::ExtractFrames` 和 `ExtractFrame` 新 content 参数已贯穿声明、实现、时间轴和编辑器调用。时间轴异步提帧保留共享 cancel 标志，回到主线程使用 weak guard，写入前再次检查取消和索引范围。
- 选择视频封面的异步结果经 parent guard 处理，避免编辑器关闭后访问已销毁 UI；GIF 预备数据返回也走 weak guard。
- 上游 Clip::Manager 的 `_promoted`、deletePrivate 和 load-level 记账配套保留：未加入工作列表的 ReaderPrivate 在持锁移除后回收，已加入的由工作线程路径处理。该审查未模拟线程竞态。
- 内联 GIF 使用测得的 realVideoSize；无法在 VideoData 中保存测量值时通过 `_inlineOverCap` 记住拒绝状态，避免持续重建超限播放器。Overview 对超限 clip 也保留拒绝标记。
- VideoUserpicPlayer 的 paused 参数及调用方已采用上游接口，暂停时不再 markFrameShown。WEB proxy 的 bridge URL/path 生成与两种 carrier 的来源路径校验成对更新。
- 本地 MPV、seek generation、取消、EOF 和 stop/join 相关文件在此次仅本地路径保留比较中保持不变；此次静态检查没有触发真实网络、MPV 或客户端。

## 生成链与条件分支

- 28 个新增 C++ 源/头、114 个动画资源引用、74 个 style/palette 模块和 921 个改动样式图标引用均有静态闭环记录。
- 204 个本地 C++ 文件的直接 quoted include/style 可见性检查补出两处 `styles/style_basic.h`。Packer 的两个既有私有签名 include 是受宏控制的外部输入，没有读取其内容。
- 正常 app 与测试入口的 UiIntegration 都具备 fontsCacheFolder；Qt 5 分支不引用 Qt 6 KineticScroller 符号。旧滚动 ID 仅保留在主仓库兼容边界，没有修改 lib_base 的 options 实现或二进制设置序列。
- rlottie 的 gitlink、注册和链接目标已移除，tlottie/Rust/Qt 配方与六个子模块指针配套；没有凭本地未构建的工具链声称编译成功。

完整门禁、证据与未执行回归见 [verification.md](verification.md)。此次审查未发现 F01–F08 以外需要改变既定方案的新实质合并冲突。
