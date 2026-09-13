# v7.2.8 上游合并记录

状态：`approved-push`。F01–F08 已按用户批准的推荐方案合并与适配，源码静态门禁与记录检查通过。本轮已有候选保存在独立目录的 `upgrade/v7.2.8` 分支；按最新指令直接推送 `ked33/merge`，确认远端 SHA 后切换到本地 `merge` 并删除旧升级分支。当前仍为提交前快照，后续实际交付结果见 [state.json](state.json)。

## 固定输入与候选

| 字段 | 本轮值 |
| --- | --- |
| 原 checkout | `D:/D-Software/source/Telegram Desktop/tdesktop - demo` |
| 原分支 / tracking | `merge` / `ked33/merge` |
| source_head / 当前升级 HEAD | `efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11` |
| 最近官方合并提交 | `e800800df3119657b2d95289cb060cd67bdba583` |
| 实际官方 parent / merge-base | `2f41383dddd338fe17fd4711afd02688c418fd47`（v7.2.5） |
| 固定目标 / MERGE_HEAD | `272f6f5c2d29d8cdb3aec15907d616b87451a3ca`（v7.2.8） |
| 官方 remote | `telegramdesktop` / `https://github.com/telegramdesktop/tdesktop.git` |
| 官方 Release | <https://github.com/telegramdesktop/tdesktop/releases/tag/v7.2.8>，正式版，2026-09-10 发布 |
| 隔离 checkout | `D:/D-Software/source/Telegram Desktop/tdesktop-upgrade-v7.2.8` |
| 当前本地分支 / 完成后 | `upgrade/v7.2.8` / `merge`，推送成功后删除旧分支 |
| 直接推送目标 | `ked33/merge` |
| 候选交付远端 | `ked33` / `https://github.com/ked33/tdesktop.git` |
| 公开产品版本 / UpstreamVersion | `7.0.9` / `7.2.8` |
| 已检查的源码索引树 | `ff168916ac290361c67d44e2e17b29c69901246d` |
| 已验证的候选 commit | 无；索引树不是提交，当前仍保留 MERGE_HEAD |

官方 tag、完整 SHA、Release 和共同祖先已在准备阶段确认，整轮目标保持不变。`upstream` remote 指向 TDesktop-x64，未将其作为正式合并来源。

原目录的一项既有文档删除、HEAD、status、索引/配置及 56 个其他受保护文件保持原状；仅按用户新指令同步修改 `docs/upstream-merge-guide.md`，原目录的 55 个未跟踪文档仍未暂存。独立 clone 无 hardlinks/alternates，39 个递归子模块有独立且干净的工作树。证据见 [source-snapshot.json](source-snapshot.json)、[source-preservation.json](source-preservation.json) 和 [submodule-preparation.json](submodule-preparation.json)。

## 合并范围与决策

v7.2.5 到固定目标累计 **144 个官方提交、233 个路径、+11,551 / -1,841 行**：33 个新增、199 个修改、1 个删除，无文件重命名或复制。与本地累计 406 个修改路径的交集是 52 个。现代三方预演识别的 **10 个冲突文件、13 个冲突块已全部解决**。

范围与原始冲突证据保留于 [inventory.json](inventory.json)、[preflight.json](preflight.json) 和 [conflict-regions.json](conflict-regions.json)。这些文件保留预检事实；最终结果以本记录和 [verification.md](verification.md) 为准。

用户明确授权原话为：**“按 F01–F08 推荐方案合并”**。详细三方依据、实际实现及各项验证见 [decisions.md](decisions.md)。

| 编号 | 已完成的适配 | 验证边界 |
| --- | --- | --- |
| F01 | 保留 64Gram 身份、公开版本 7.0.9；只将 UpstreamVersion 改为 7.2.8 | 字段/对象比较通过，未做安装和更新服务验证 |
| F02 | 合并上游文件夹切换与本地补搜/重试三个订阅，保留动画和滚动恢复 | 订阅、lifetime 和接口检查通过，未运行 UI |
| F03 | 保留截图模式刷新、搜索高亮和频道标记；采用新手势方向计算 | 双绘制路径和本地选择/编辑函数体检查通过 |
| F04 | GL 容量从 kControlsOffset 推导，含 wheel hint；保留亮度/镜像和 RHI 修复 | 560 float / 2,240 字节边界算术通过，未做 GPU 回归 |
| F05 | 保留本地 FLOOD 延迟/覆盖/通知，加入上游 RPC 重试观测 | 单一延迟分支及局部语法检查通过，未发网络请求 |
| F06 | 默认 v1；v2 命名、最低版本与官方签名预检仅在显式开关下启用 | 28 组跨平台制品名检查通过，未打包/签名/部署 |
| F07 | 更新六个子模块和 tlottie/Rust/Qt 配方，保留 tgcalls/lib_storage 定制 | 39 个递归子模块一致，生成链静态通过，未编译依赖 |
| F08 | Qt 6 新惯性滚动名称/说明；在启动及导入边界兼容旧布尔键，新键优先 | Qt 5/6 条件和 27 组静态分支预期已审查，未运行真实迁移 |

QR 子模块的 18 个 Java 文件在用户确认火绒拦截并放行升级目录后恢复，工作树已 clean。expected-lite 的一次 Schannel 握手失败通过单次命令选择 OpenSSL 重试解决；未修改全局 TLS 配置。

## 模块与保留检查

| 模块 | 已完成 | 剩余验证 |
| --- | --- | --- |
| G01 构建、子模块、生成链 | F06/F07、28 个新增 C++ 源/头注册、114 个动画引用、74 个样式模块和 921 个图标引用 | 同一候选 SHA 的云编译及依赖准备 |
| G02 选择、菜单、转发 | 本地选择/快速复制函数体保留、TopBar weak/menu lifetime、新窗口接口 | 跨聊天/评论选择、菜单 hover、受限媒体 |
| G03 History、Compose、搜索 | F02/F03、双路径绘制、附件第三参数、目录归档、编辑导航保留 | 搜索、手势、编辑/GIF/目录发送 |
| G04 媒体、streaming、网络 | F04/F05、提帧回调/取消、Clip 生命周期、Smart 策略脚本 | GPU、线程、网络等待及 MPV/seek |
| G05 设置、语言、旧配置 | F08、英中文资源、启动先 set 后 init、导入校验前迁移 | 旧 portable、导入/导出与重启 |
| G06 品牌、版本、更新器 | F01/F06、产品资源、默认 v1 及显式 v2 限制 | 安装覆盖、真实制品/签名和更新服务 |

最终对象比较覆盖 354 个 local-only 与 181 个 upstream-only 路径：仅有 F08 两份本地化、两处直接 style include，以及 F06 四个 shell/Python 发布脚本的计划内差异，无非预期差异。完整结果见 [final-preservation.json](final-preservation.json)、[adaptation-checks.json](adaptation-checks.json) 和 [automatic-review.md](automatic-review.md)。

功能正文与历史附录已同步：[local-changes.md](../../local-changes.md)、[local-changes-history.md](../../local-changes-history.md)。三个本地作者的可达非 merge 提交主表补齐为 **330 条**，完整机器记录见 [local-commit-audit.json](local-commit-audit.json)。

## 验证与下一步

```text
static: pass
cloud_build: not-run
runtime: not-run
commit: not-created
integration_push: not-applicable
push_target: ked33/merge
local_upgrade_branch_cleanup: pending remote verification
publish_to_ked33_merge: not-run
```

源码快照有 235 个改动路径，223 个文本文件编码/格式检查通过；Python AST、XML/JSON/YAML、shell 语法、资源、样式、子模块及 31 项重点适配检查通过。既有 Smart 策略脚本的公式和 56 项结构检查全部通过。8 个手工适配 C++ 文件的 Qt 5/6 解析没有新增诊断，仍不代表 C++ 类型或链接通过。

源码索引树是在暂存新文档之前生成的，含完整合并源码和上游随附文档；新增的本轮记录、功能正文、历史附录及指南已另行检查和暂存。详细证据、范围及未执行回归见 [verification.md](verification.md)。

本机没有可依赖的 Telegram C++ 构建环境，本轮没有本地构建或客户端运行。现有 `build-win-x64.yml` 会响应升级分支 push，使用 Release；当前没有触发该流程。

用户已回复“执行push”，并进一步要求直接推送 `merge`、推送成功后删除本地 `upgrade/*` 分支；本轮 commit、`ked33/merge` push 和相应分支清理均已授权。现有 Windows x64 Release 云构建在推送后触发，运行验证仍待完成。下文及静态报告中的“未提交”字段是提交前快照，实际交付结果随后记录于 state.json。

## 后续关闭字段

```text
source_head = efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11
official_upstream_anchor = pending committed merge; target 272f6f5c2d29d8cdb3aec15907d616b87451a3ca
next_preflight_base = pending completed merge; recompute from committed history
integration_branch = upgrade/v7.2.8
push_remote = ked33
push_branch = merge
pushed_candidate_sha = pending push
local_upgrade_branches = pending successful push and cleanup
verified_candidate_sha = none
source_index_tree = ff168916ac290361c67d44e2e17b29c69901246d
local_head_after = efa2a4c9c8d1fd6f6a84a57b8c6c363a053ccb11
remote_head_after = not-published
runtime_status = not-run
open_risks = cloud build, Qt5 compile coverage, portable/UI/media/network regression, packaging/update service
```
