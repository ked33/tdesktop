# v7.2.8 合并候选验证

F01–F08 已按用户批准的推荐方案完成，源码静态门禁通过。本报告保留 `upgrade/v7.2.8` 的**提交前合并索引**及静态验证证据；该快照阶段没有提交、推送、云构建或客户端运行。按最新指令，本轮直接推送 `ked33/merge`，成功后切换到本地 `merge` 并删除旧升级分支；实际交付结果见 [state.json](state.json)。

## 验证对象

| 字段 | 值 |
| --- | --- |
| source_head / 提交前 HEAD | `efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11` |
| 提交前 MERGE_HEAD / 固定官方目标 | `272f6f5c2d29d8cdb3aec15907d616b87451a3ca`，v7.2.8 |
| merge-base | `2f41383dddd338fe17fd4711afd02688c418fd47`，v7.2.5 |
| 源码索引树 | `ff168916ac290361c67d44e2e17b29c69901246d` |
| 源码快照范围 | 相对 source_head 的 235 个路径，含上游随附文档；新加入的本轮记录、功能正文、历史附录和指南不在该树中 |
| 已验证的候选 commit SHA | 无；上面的 tree SHA 不是提交 |
| 公开产品版本 / UpstreamVersion | 64Gram 7.0.9 / 7.2.8 |

新增记录的最终暂存状态由 [state.json](state.json) 与 [documentation-checks.json](documentation-checks.json) 记录。源码文件的 SHA-256、生成链检查和索引树见 [static-checks.json](static-checks.json)。

## 静态门禁

| 检查 | 结果与范围 | 证据 |
| --- | --- | --- |
| 合并冲突 | 最初 10 文件、13 块；索引无未解决条目 | [预检](preflight.json)、[实际状态](state.json) |
| Git 空白检查 | 工作树及暂存差异均通过 | [基础检查](static-checks.json) |
| 编码与标记 | 223 个源码快照文本文件为 CRLF、无 BOM，无冲突标记；5 个二进制资源单独计数 | [基础检查](static-checks.json) |
| Python / XML / JSON / YAML / Shell | 6 个 Python 文件通过 Python 3.10 语法 AST；3 个 XML/SVG/QRC、2 个 JSON、1 个 YAML 解析通过；3 个 shell 通过 `bash -n` | [基础检查](static-checks.json) |
| 新源文件与资源 | 28 个新增 C++ 源/头在 Telegram 或 td_ui 中各注册一次；animations.qrc 的 114 个引用均存在 | [基础检查](static-checks.json) |
| Style 生成链 | 74 个 style/palette 模块，using 闭包、注册、全局变量/类型重复检查通过 | [样式检查](style-audit.json) |
| Style 可见性 | 审核 204 个本地 C++ 文件、3,368 个已知 st:: 引用；补上 highlight manager 和 streaming utility 的直接 style_basic 头 | [样式检查](style-audit.json) |
| 图标 | 6 个改动 style 中的 921 个文件引用及 4 个程序生成尺寸项已核对；路径、修饰符、SVG XML、PNG 1x/2x/3x 及尺寸配套通过 | [样式检查](style-audit.json) |
| 子模块 | 39 个递归子模块 HEAD 与 gitlink 一致、内部 clean、无 alternates；六个更新指针和两个本地定制指针符合 F07 | [子模块收据](submodule-preparation.json)、[指针清单](submodules.json) |
| 版本与重点适配 | 31 项结构、函数体保留和边界算术检查通过 | [适配检查](adaptation-checks.json) |
| 跨脚本更新制品 | v1/v2 × stable/beta × 各平台共 28 组命名配套通过；最低版本限制仅在显式 v2 | [制品检查](artifact-name-checks.json) |
| Smart streaming | 既有策略脚本的公式与 56 项结构检查全部通过 | [执行收据](smart-policy-check.json) |
| 局部 C++ 语法 | 8 个手工适配文件 × Qt 5.15.19/6.11.2 分支，没有新增解析诊断 | [语法检查](cpp-syntax-audit.json) |
| 本地功能保留 | 354 个 local-only、181 个 upstream-only 路径的对象比较无非预期差异 | [保留检查](final-preservation.json) |
| 原目录保护 | 原 HEAD、status、索引/配置及 56 个其他受保护文件保持原状；仅按用户指令同步更新合并指南 | [原目录检查](source-preservation.json) |

语法解析器不是编译器；上述检查不证明 C++ 类型、模板实例化、MSVC 链接或 Qt 生成器执行成功。Style 检查没有运行 Qt codegen 或渲染 SVG。Packer 的两个既有外部签名头按 `PACKER_DISABLE_PRIVATE` 条件归类，没有读取私钥文件。

旧符号检查未发现主仓库对 `kOptionQScroller`、`SetupScrollerPhysics`、`ResendScrollerPrepare`、`ScrollerStopper`、旧 `QFixed` 类型或 rlottie 构建目标的残留使用。`qscroller` 只保留于明确的兼容读取/迁移处。上游 `lib_ui/ui/text/text_shaper_qt.cpp` 内部仍使用 Qt 私有 `QFixed`，这是新整形边界的正常实现；`Ui::kQFixedMax` 也不是被删除的类型，不作误报处理。

## 重点结论

- **品牌与更新**：header 只改变 UpstreamVersion；build/version、RC、AppX、setup.iss、生产 update checker 与 Packer 保持本地版本。默认仍走 v1，Windows x86/x64 使用 64Gram 安装包名。未执行完整打包、签名、上传或部署脚本。
- **搜索与消息**：文件夹切换、补充搜索设置、重试三个订阅均存在并绑定 Widget lifetime。保留截图模式刷新，采用上游双方向手势。两条 History 绘制路径及本地选择、快速复制和 Compose 编辑导航已复核。
- **GL/RHI**：固定区从 kControlsOffset 得到 48 个顶点；固定区、控件、圆角、Stories 共 560 个 float，即 2,240 字节。最后一个 sibling 写入终点与分配终点相同。保留 wheel hint、亮度与镜像；RHI 隐藏帧空 pass 修复已合入。
- **RPC**：除上游 constructor ID 观测和缩进外，本地等待分支与 source_head 一致；没有产生第二套 secs、退避或延迟插入逻辑。保留原有去重路径、服务端默认等待、覆盖值及三项通知字段。Debug 实现/Release no-op 均由同一 test_rpc_retry 源文件提供。
- **依赖**：rlottie 已移除；采用 tlottie 4b940c7942、Rust 1.96.1、Qt 6.11.2 / Qt 5.15.19 配方。新 Rust/Qt/tlottie 工具链尚未构建；配置中的版本和路径闭合不代表构建成功。

各自动合并接口、回调和生命周期的最终审查范围见 [automatic-review.md](automatic-review.md)。

## 旧滚动设置：静态分支矩阵

完整 27 组构造 JSON 的预期结果见 [experimental-migration-matrix.json](experimental-migration-matrix.json)。这是依据最终 C++、base/options.cpp 和 Qt 条件编译进行的静态分支审查，**没有执行 C++ 迁移，也没有读取或改写真实用户配置**。

| 输入 | Qt 6 启动读取 | Qt 6 导入 |
| --- | --- | --- |
| 只有旧 qscroller，值为 true / false | 应用旧布尔值 | 先改为新键，再走原导入流程 |
| 只有新 kinetic-scroller，值为 true / false | 使用新值 | 使用新值 |
| 新旧布尔键同时存在，包括冲突 | 新键优先 | 新键优先 |
| 新键存在但类型错误，旧键有效 | 不回退旧值，使用平台默认值 | 类型校验失败，保留导入前的设置 |
| 旧键为字符串、数字、null、数组或对象 | 忽略旧值，使用平台默认值 | 旧键按未知选项忽略，沿用原成功导入的 reset/apply 语义 |
| 两键缺失 | 使用平台默认值 | 沿用原成功导入的 reset/apply 语义 |
| 错误 JSON 或根不是对象 | 原初始化记录错误并保留默认值 | 拒绝，不 reset 已有选项 |

Qt 6 默认值来自上游：Windows/macOS 为 false，其余平台为 true。Qt 5 不注册新旧滚动选项，不引用 Qt 6 专用符号；有效对象中的这两个键按未知键处理，错误 JSON/非对象仍被原导入器拒绝。

启动在 `base::options::init(path)` **之前**应用旧布尔值。这时 LocalPath 尚为空，不会安排依赖 QCoreApplication 的保存计时器；随后正常初始化其他选项。启动兼容读取不会立即改写旧文件。之后的正常保存/导出只使用已注册的新键，值等于默认值时按既有策略省略。

## 尚未执行的验证

| 范围 | 后续必须记录的实际结果 |
| --- | --- |
| 云编译 | 同一候选 commit 的 Windows x64 Actions run URL、head_sha、结论和 artifact；Qt 5 分支目前也只有静态审查 |
| 启动与旧 portable | 登录、旧实验设置、旧键迁移、新旧键冲突、导入/导出、重启保持 |
| 搜索与聊天 | 普通/特殊搜索、重试与限流、结果进度/导航、快速切换文件夹及恢复滚动位置 |
| 选择与编辑 | legacy HistoryWidget 和新 Compose/ListWidget，跨聊天/评论拖选、快速复制、selected-copy、编辑导航、截图模式和频道标记 |
| 图形与媒体 | GL/RHI、wheel hint、亮度/镜像、Stories、透明内容、关闭重开、首播/seek/暂停/退出 |
| 本地 streaming/MPV | Smart/Soft Seek、普通/特殊 MPV、分片回跳、取消请求、EOF、线程退出、FLOOD 等待及通知 |
| 上游新增功能 | TGS/emoji/sticker、字体选择/复制、视频/GIF 编辑、目录归档、菜单关闭/hover、视频头像暂停、WEB proxy 路径 |
| 其他本地功能 | 翻译后端、缓存及链接/代码保护、菜单/Details/no-forwards、播放 profiles、emoji/sticker 尺寸 |
| 品牌与更新 | 安装覆盖、旧 portable、默认 v1 制品及真实签名/更新服务；本轮未启用 v2 |

本机没有可依赖的 Telegram C++ 构建环境，本轮未进行本地构建或客户端运行。现有 Windows x64 workflow 对 push 无分支限制，构建类型是 Release；提交前静态快照尚未触发它。按用户最新指令，静态检查通过后直接推送 `ked33/merge`，再跟踪云构建和运行回归。不能将未执行项记为通过，也不能仅因已经推送而关闭本轮。
