# v7.2.8 冲突与适配方案

基线固定为 Base=`2f41383dddd338fe17fd4711afd02688c418fd47`、Local=`efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11`、Upstream=`272f6f5c2d29d8cdb3aec15907d616b87451a3ca`。以下行号若未特别注明，来自预演树；原始片段在 [conflict-regions.json](conflict-regions.json)。涉及的文件均以这三个对象重新比较，未复用上轮冲突结论。

用户已明确批准：**“按 F01–F08 推荐方案合并”**。以下保留原三方依据和风险分析；各项源码适配已完成，末尾状态已更新为实际静态结果。候选仍未提交，云编译和运行验证尚未执行，完整证据见 [verification.md](verification.md)。

## F01：产品身份与公开版本

路径：`Telegram/SourceFiles/core/version.h`（AppId/AppName/AppFile/AppVersion/UpstreamVersion）、`Telegram/build/version`、`Telegram/Resources/winrc/Telegram.rc`、`Telegram/Resources/winrc/Updater.rc`、`Telegram/Resources/uwp/AppX/AppxManifest.xml`。

- Base：官方产品版本 7.2.5；Local：64Gram 品牌，公开版本 7.0.9，UpstreamVersion=7.2.5；Upstream：官方产品版本 7.2.8。预演中 version.h 第 23–33 行和四个资源文件共有 7 块冲突。
- 本地意图：维持产品身份、安装覆盖关系、公开版本和既有更新服务约定。指南第 4 节已定义默认政策。
- 直接选 Local：UpstreamVersion 仍会停在 7.2.5。直接选 Upstream：覆盖 64Gram 名称、可执行文件名和公开版本。
- 推荐：按字段保留本地身份、版本和 RC 字符串，仅将 `UpstreamVersion` 改成 `7.2.8`；保留 AppX 原有身份字段，不借本轮重命名产品。不要整文件 ours/theirs。
- 检查：header、build/version、RC、AppX、setup.iss 的版本/产品字段；旧 portable、安装覆盖和更新服务均需另行运行验证。
- 风险：中。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：字段及对象比较通过：仅 UpstreamVersion 从 7.2.5 改为 7.2.8，其他本地身份资源一致；未执行安装/更新回归。

## F02：补充搜索与文件夹切换

路径：`Telegram/SourceFiles/dialogs/dialogs_widget.cpp` 构造函数（预演第 458–470 行），以及 `dialogs_inner_widget.cpp/.h`、`dialogs_widget.h`、`dialogs.style`。

- Base：`InnerWidget` 自己订阅 `activeChatsFilter`。Local：外层 Widget 创建 InnerWidget 后新增 `SearchIncludePornChanges`、`retryPornSearchRequests` 订阅，保留特殊搜索、固定进度和重试入口。
- Upstream：把文件夹订阅移到外层 Widget，经 `switchToChatsFilter()` 抓取前后画面并启动滑动动画；InnerWidget 的旧订阅已删除，滚动恢复返回 bool，缺少状态时回到顶部。
- 直接选 Local：内层旧订阅已经被自动删除，文件夹切换可能不再生效。直接选 Upstream：补搜设置刷新和重试订阅丢失。
- 推荐：在 InnerWidget 创建后先接好上游的 activeChatsFilter 订阅，再保留两个本地订阅；都绑定 Widget lifetime。保留本地搜索数据/进度/结果导航及上游动画、滚动恢复，不增加第二个文件夹订阅。
- 检查：三类订阅各一处，switchToFilter 的声明/访问级别/返回值完整；普通/特殊搜索、重试、快速切换文件夹、隐藏全部聊天、销毁窗口后回调。
- 风险：中。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：三个指定订阅各一处并绑定 lifetime；文件夹接口配套。见 adaptation-checks.json；未运行搜索/切换 UI。

## F03：截图模式与手势方向

路径：`Telegram/SourceFiles/history/view/history_view_message.cpp` 的 `Message::draw()`（预演第 2275–2286 行），以及 `history_inner_widget.cpp`、`history_view_list_widget.cpp`、`ui/controls/swipe_handler_data.h`。

- Base：`hasGesture` 和原始 translation 控制消息位移。Local：在回复绘制后刷新 `_previousMode`，供截图模式下发送者/转发者名称重绘使用；另有搜索高亮和频道身份标记。
- Upstream：引入 `visualTranslationFor()`、`inverted`，用 `gestureShift` 统一位移、反向恢复、反应动画偏移和镜像回复图标；绘制开头已自动改名。
- 直接选 Local：保留的 `hasGesture` 已无对应声明，且会破坏新方向计算。直接选 Upstream：丢失截图模式状态刷新。
- 推荐：保留完整截图模式刷新，随后使用上游 `gestureShift` 分支；保留自动合入的 via-bot 名称宽度和点击区域修复，并核对本地搜索高亮、频道标记及编辑选择范围仍存在。
- 检查：draw 中无旧 hasGesture；位移恢复对称；两条 History 绘制路径、截图模式切换、长发送者名/via bot、搜索结果高亮、左右滑动与回复图标。
- 风险：中。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：新手势位移/恢复对称，截图模式保留；两条选择/编辑路径的重点函数体与 source_head 一致。未运行绘制/选择回归。

## F04：媒体查看器顶点缓冲

路径：`Telegram/SourceFiles/media/view/media_view_overlay_opengl.cpp` 的 offset 常量和 `RendererGL::init()`（预演第 184–188 行），关联 RHI renderer。

- Base：固定 `kQuads=9`，实际 controls 起点需要 11 个 quad。Local：新增 `kWheelHintOffset`，将值改为 10，保留亮度与镜像绘制。Upstream：修复原始容量不足，将 9 改为 11；offset 链本身没有新增本地提示区。
- 直接选 Local：继续保留上游已修复的缓冲不足。直接选 Upstream：少算本地 wheel hint 的一个 quad；缓冲尾部绘制仍可能越界。
- 推荐：移除独立的 kQuads 硬编码，令 `kQuadVertices = kControlsOffset`，从实际布局推导。当前本地布局为 48 个顶点，即 12 个 quad。保留后续 controls、rounding、stories sibling 缓冲计算和本地亮度/镜像；RHI 隐藏帧清屏修复采用上游。
- 检查：每个绘制偏移、controlsCount、rounding 和 stories sibling 写入上界均不超过分配值；GL/RHI、滚轮提示、缩略图、透明内容、镜像、关闭后重开。
- 风险：高。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：固定区 192、controls 256、rounding 48、siblings 64，共 560 float / 2240 bytes，最大写入终点不超分配；RHI 修复保留。未做 GPU 验证。

## F05：RPC 重试观测与 FLOOD 等待

路径：`Telegram/SourceFiles/mtproto/mtp_instance.cpp` 的 include 与 `Instance::Private::onErrorDefault()`（预演第 29–34、1510–1552 行），`test/test_rpc_retry.*`，CMake 注册。

- Base：负错误码/5xx 指数退避，FLOOD 根据服务端等待值延迟。Local：保留 `flood_premium_wait_override_ms`，默认空字符串按服务端等待；通知携带 serverWaitSeconds、overrideWaitMs、appliedWaitMs，支持本地 Smart/上传下载诊断。
- Upstream：对负码/5xx 从请求表读取 constructor ID，调用 `Test::RecordRpcRetry`；观测实现放在 test/，Release 分支为 no-op。
- 当前预演会产生重复 secs/nonPremiumDelay 和两段退避逻辑，仅删除标记无法得到正确代码。直接选 Local 会遗漏观测；直接选 Upstream 会丢失本地等待覆盖及通知结构。
- 推荐：保留一个完整的本地延迟计算/插队去重/通知分支，在其负码/5xx 分支开始加入上游观测，保留 settings.h、algorithm 与 test_rpc_retry.h 三个 include。只记录 constructor ID，不记录请求内容；不改变 FLOOD 默认值，不新增生产 debug 状态。
- 检查：负码/5xx 只退避一次，普通 FLOOD、Premium FLOOD、短 slowmode，重复 delayed request，通知字段和单位，Smart/上传下载调用链，test helper 在 Debug/Release 的链接闭环。
- 风险：高。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：除上游观测及缩进外，完整本地等待分支与 source_head 一致；单一 secs、退避、插入和通知路径，helper 注册闭合。未运行网络/延迟回归。

## F06：fork v1 更新和官方 v2 发布脚本

路径：`Telegram/build/build.bat`（预演第 130–159 行及冲突外自动变化）、`build.sh`、`deploy.sh`、`release.py`、`release.sh`、`sign_update.py`；核对 `setup.iss`、`core/update_checker.cpp`、Packer 的生产接受策略。

- Base：各平台默认经典 v1，`TDESKTOP_UPDATE_V2=1` 才使用官方 v2 制品名和签名流程。Local：Windows x64/x86 使用 64Gram 安装包名；公开版本 7.0.9；生产更新保留 fork HTTP 来源和本地 RSA 验签，不接受官方根签发的 v2 包。
- Upstream：正式发布默认全部使用 v2，移除 v1 分支和环境开关，增加 AppVersion >= 7002000 检查、官方本地签名文件/Azure 密钥预检、构建目录锁、portable 覆盖保护和签名重试。所有平台的发布脚本都受影响，不能只修 build.bat 的冲突块。
- 直接选 Local 冲突块：冲突外仍有强制签名和最低版本拒绝，也仍会覆盖制品名。直接选 Upstream：7.0.9 的本地打包立即被拒绝，并依赖官方签名设施；生成包也不符合 fork 当前生产接受策略。
- 推荐：保留默认 v1 分支、fork 产品版本和 Windows x64/x86 安装包名，配套核对 build/deploy/release 的消费名称；显式 v2 分支才使用新命名、最低版本和签名预检，并让不满足条件的 v2 请求明确失败。本轮不启用 v2，不修改真实密钥、签名资源、代理或账户配置。吸收上游独立的构建锁、portable 覆盖检查、macOS 打包图标修复及 sign_update.py 通用检查能力。
- 在 Linux/macOS 脚本中也保留同一默认 v1 政策；不能让 Windows 产物和上传脚本消费名不一致。脚本只做静态解析与路径/分支检查，不执行打包、签名、上传或部署。
- 检查：默认路径无需官方私钥/Azure 会话，版本检查仅属于 v2；x86/x64/ARM 与 stable/beta 名称一致；锁释放/退出码和签名失败路径；生产 v1 完整验签及官方 v2 拒绝策略未被扩大。实际安装包、签名服务、更新服务器仍需独立验证。
- 风险：高。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：28 组跨平台 v1/v2、stable/beta 制品名称配套通过；五处最低版本门禁仅限 v2；生产 update checker/Packer 保持本地版本。未执行完整打包/签名/上传/部署。

## F07：tlottie、Qt 与配套子模块

路径：`.gitmodules`、`Telegram/CMakeLists.txt`、`Telegram/build/prepare/prepare.py`、`qt_version.py`、Docker/Snap 构建配方、`Telegram/lib_base`、`lib_lottie`、`lib_spellcheck`、`lib_ui`、`lib_webview`、`cmake`。

- Base/Local：rlottie 独立子模块；六个待更新依赖的 Local 与官方基线相同。另有 tgcalls、lib_storage 两个本地定制指针，此区间上游未更改它们。
- Upstream：删除 rlottie gitlink/注册/主程序链接，lib_lottie 改用 tlottie，cmake 增加 external_tlottie；prepare 新增独立 Rust 1.96.1 和固定 tlottie 4b940c7942；Qt 6 更新到 6.11.2，Qt 5 仍为 5.15.19。lib_ui 重构字体整形、触控滚动和字体缓存接口；spellcheck/webview 配套更新。官方最终目标已包含 MSVC 及 macOS 构建配方修复。
- 直接保留旧依赖会与新主仓库 API/链接目标不一致，并漏掉本版 Lottie 修复。无条件采用官方所有 gitlink 则会覆盖没有上游变化的 tgcalls/lib_storage 定制。
- 推荐：更新六个发生上游变化的依赖，完整采用 tlottie/Qt/生成链；保留两个本地独有 gitlink 和现有 fork URL。lib_base/cmake 的目标 SHA 已经通过当前 fork URL 精确 fetch 成功，其他目标来自其声明的官方来源。
- 证据：[submodules.json](submodules.json)。六个独立子模块副本已同步到批准的目标 gitlink；递归 39 个子模块的 HEAD、clean 状态和无 alternates 证据见 [submodule-preparation.json](submodule-preparation.json)。新 Rust 工具链只由未来构建流程按配方准备，本机未安装或运行构建。
- 检查：rlottie 在生产/CMake 无残留；tlottie 头/库输出与 imported target、Windows 系统库一致；新增源文件/style/QRC 注册；ui fontsCacheFolder、Fixed、KineticScroller 等声明与调用闭合；递归子模块 HEAD 与 gitlink 一致。旧 portable、TGS/emoji/sticker、字体选中/复制和 WEB proxy 需云编译及运行回归。
- 风险：高。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：39 个递归子模块 HEAD/gitlink 一致且内部 clean；CMake/资源/style 和直接 include 检查通过。Rust/Qt/tlottie 工具链未实际构建。

## F08：实验滚动设置迁移及本地化

路径：`Telegram/SourceFiles/settings/settings_experimental.cpp` 的 `ExperimentalOptionName()` / `ExperimentalOptionDescription()`（预演第 168、275 行），`core/launcher.cpp` 的 options 初始化，`Telegram/Resources/langs/lang.strings`、`localization/en.json`、`localization/zh-CN.json`。

- Base：lib_ui 定义 `Ui::kOptionQScroller`，保存 ID 为 `qscroller`。Local：增加上述两处名称/说明映射和中英文资源，支持实验选项导入/导出。
- Upstream：删除旧声明，Qt 6 使用 `Ui::kOptionKineticScroller`，保存 ID 为 `kinetic-scroller`；默认仍为 all-other 平台开启、Windows/macOS 关闭，Qt 5 不注册新选项。上游列表虽已自动改好，本地两个映射分支仍留着旧符号，确定会造成编译失败。
- 直接保留 Local：未定义符号。仅改名字而不加 Qt 条件：Qt 5 构建失败。仅删除本地映射：丢失本地化，旧 qscroller 显式选择被忽略。
- 推荐：把两处映射换成新的 Qt 6 条件分支，资源名称/说明按新惯性滚动行为更新；兼容读取旧布尔键 qscroller，仅在没有显式 kinetic-scroller 时应用旧值，新键优先。兼容逻辑放在主仓库现有实验选项初始化/导入边界，不为此修改配套 lib_base 的官方 gitlink；不改顺序二进制序列化。
- 检查：Qt 5/6 预处理条件，旧/新键单独和同时存在、true/false、缺失/错误类型、导入/导出、旧 portable 启动；资源 key 一致，旧保存键仅存在于明确兼容读取处。真实用户 experimental_options.json 不读取或改写，迁移行为先用构造数据验证。
- 风险：中。用户选择：推荐方案（已批准）；源码适配：已完成；静态验证：新增 core/experimental_options.* 并注册 CMake；启动先应用旧布尔值再 init，避免 QApplication 创建前安排保存 timer。Qt5/6 语法及 27 组静态分支预期已审查，英中 key 配套；未读写真实配置或运行 C++ 迁移。

## 自动合并边界

- HistoryWidget、ChatWidget、ScheduledWidget 的新增目录归档入口及 SendGifWithCaption 的第三个回调参数已成对自动合入；本地 editable-message navigation、跨聊天选择/快速复制的路径仍在。
- 系统外部媒体查看器的新保存限制和阅后即焚限制采用上游；本地独立 MPV 入口与内置视频帧复制实现不在本次这些修改块中，不扩展或重写其策略。
- TopBar 菜单关闭/hover 处理、视频头像暂停、WEB proxy 路径支持、视频/GIF 编辑器、FFmpeg 日志和实际帧尺寸检查采用上游，同时保留自动合并的本地功能。
- 六个依赖更新后已完成直接 include、style 可见性、重点 API 调用和旧符号检查，并补上两处 style_basic 直接 include。最终审查范围见 [automatic-review.md](automatic-review.md)，不表示客户端编译或运行通过。

## 授权与验收

本次用户已明确批准 F01–F08 推荐方案，正式合并与适配均在隔离升级目录完成；无需再次索取这些决策。最终审查没有发现需要改变既定方案的新实质合并冲突。原始预检证据保留，实际实现和验证以上述状态及 verification.md 为准。

用户已回复“执行push”，并进一步要求直接推送 `merge`、推送成功后删除本地 `upgrade/*` 分支；本轮 commit、`ked33/merge` push 和相应分支清理均已授权。现有 Windows x64 Release 云构建在推送后触发，运行验证仍待完成。云编译与运行验证只依据本次真正提交，不复用旧 Actions 结论。
