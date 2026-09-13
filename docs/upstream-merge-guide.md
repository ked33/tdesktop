# Telegram Desktop 上游合并长期运行手册

本手册适用于 `tdesktop - demo` 的长期跟进工作。它把一次性的冲突处理，固化为可以在每个官方 Release 或指定 commit 重复执行的流程。目标是：完整保留本地功能、可解释地吸收官方演进、尽量一次通过 Windows x64 云构建，并让下一次合并可以从明确的 upstream anchor 继续。

## 1. 范围、远端与事实来源

正式合并源永远是：

```text
https://github.com/telegramdesktop/tdesktop
```

本 checkout 中的 `telegramdesktop` remote 才是官方合并 remote；`upstream` 当前指向 `TDesktop-x64/tdesktop`，只能作为 64Gram 参考实现，不能作为官方基线、Release 判断或 merge target。交付远端当前是 `ked33`，推送目标固定为长期分支 `merge`。在独立检出目录的 `merge` 分支完成合并、适配和静态检查，commit/push 授权覆盖后直接推送到 `ked33/merge`，再跟踪该提交的云构建和运行回归。不再创建本地或远端 `upgrade/*` 分支；旧流程已有的本地临时分支按第 7 节在推送成功后清理。执行前每次重新确认两个检出目录的 remote、tracking 和 `remote.pushDefault`；推送时始终显式指定 `HEAD:refs/heads/merge`。

版本事实按以下优先级确认：用户明确给出的完整 SHA > 官方 tag 的 peeled SHA > 官方 Release 页面 > `telegramdesktop/dev` 分支头。分支名“最新”不能替代精确 SHA。执行时必须重新验证 tag、Release 和对象可用性；它不是已执行的合并结论。

## 2. 状态机与授权边界

每次升级记录只能处于一个状态：

```text
planned -> target-verified -> approved-preparation -> objects-ready
-> preflighted -> awaiting-decisions -> approved-merge
-> conflict-resolved -> static-verified -> approved-commit
-> approved-push -> pushed -> ci-pending -> ci-passed
-> runtime-pending -> runtime-verified -> closed
```

已有合适的隔离副本和完整对象时，可以记录事实后跳过 `approved-preparation`。`pushed` 表示 `ked33/merge` 的远端 SHA 已与本地候选提交核对一致；云构建和运行验证在这次推送后进行，不作为推送前置条件。云构建失败记为 `ci-failed`，修复后重新完成受影响的静态检查，直接推送修复提交到 `ked33/merge`，并跟踪新 HEAD 的云构建。运行验证未完成时保留 `runtime-pending`；用户明确允许跳过部分验证时，记录未验证项及接受依据，不得写成 `runtime-verified`。直接推送不等于验收完成。

`blocked` 仅表示外部条件或用户决策缺失，不表示“自动选择本地”或“自动选择上游”。四种授权必须分开记录覆盖范围，但可以在同一条指令中给出；已有明确授权时直接继续，不重复索取：

1. 准备/fetch 授权：允许在指定新目录创建独立副本并使用 `merge` 分支，定向获取已确认的官方 tag/SHA 及所需子模块对象；这不授权正式合并或推送。
2. 冲突决策/合并授权：允许在隔离副本的 `merge` 分支对已经列出的冲突采用指定方案并合入固定目标。
3. commit 授权：允许创建指定隔离副本中的合并或适配提交。
4. push 授权：默认目标为 `ked33/merge`，覆盖静态检查通过后的直接推送，以及本次升级范围内必要修复提交的推送。用户已要求执行 push 时，按该目标继续；不再拆成“升级分支推送”和“长期分支发布”两次确认。用户当次明确指定其他目标或限制时，以当次指令为准。

准备授权前只读；对象缺失时先用官方 API、`ls-remote` 或完整 diff 流确认目标，再申请缺少的准备授权，无需等全部冲突决策完成后才 fetch。获批准备操作只在指定隔离副本进行，原 checkout 的源码、索引、配置、子模块及脏文件保持原状。

完成准备后，预检仍不修改业务源码或索引；分析记录仅写入当次已授权的记录目录，严格只读任务则先在对话中输出。正式合并前收齐已识别实质冲突的决策。用户“全部按推荐方案合并”只覆盖报告中已经列出的冲突，不覆盖后来发现的新语义冲突；新冲突暂停相关适配并补充报告，其他独立的只读分析可以继续。

## 3. 基线与冲突预检

### 3.1 工作区快照

记录以下输出或等价信息：

```bash
git status --short --branch
git branch -vv
git remote -v
git config --get remote.pushDefault
git config --get branch.merge.remote
git config --get branch.merge.merge
git rev-parse HEAD
git log -1 --format='%H%n%P%n%ad%n%an <%ae>%n%s' --date=iso-strict
```

在原 checkout 与隔离副本分别记录快照；示例中的 `branch.merge.*` 应按被检查的实际分支替换。既有脏文件必须列入 `worktree_baseline`。例如当前已知的 `Telegram/SourceFiles/media/streaming/tests/__pycache__/` 不得误删、暂存或归因于本次升级。

子模块显示修改时，分别核对父仓库记录的 gitlink、子模块工作树 HEAD 和子模块内部改动。单纯处于不同提交不等于存在源码修改，也不能直接清理或覆盖。

### 3.2 目标与范围

先固定 `source_head`、最近一次实际合入的官方 SHA 和 `target_sha`。对象已经存在时使用 Git 计算；对象缺失且未获准备授权时，仅用官方 API、`ls-remote` 或 diff 流收集事实。准备获批后，在隔离副本定向 fetch，再核对对象、tag 指向和共同祖先，完成本地 Git 预检：

```bash
git show-ref --verify refs/tags/<tag>
git rev-parse <target>^{commit}
git merge-base HEAD <target>
git log --oneline --reverse <previous-upstream>..<target>
git diff --stat <merge-base>..<target>
git diff --name-status --find-renames --find-copies <merge-base>..<target>
```

GitHub Compare 的文件列表可能被截断（曾出现 300 文件上限），大范围升级必须用完整 `.diff` 流或本地 Git 结果交叉核对。单独记录：提交数、文件数、增删行、重命名/移动、删除、新增、子模块 gitlink 变化和生成链变化。

整轮升级保持目标 SHA 不变。期间发布的新版本另开下一轮，不能在未重新预检时替换本轮目标。对象尚未获取时必须标明范围或冲突结论的限制，不能把远程文件交集称为完整三方合并结果。

### 3.3 冲突预判

若目标对象已存在，在绝对只读阶段使用三参数形式 `git merge-tree <merge-base> <source-head> <target>` 做初筛；该旧式模式不能替代正式合并对重命名等情况的处理。`git merge-tree --write-tree --messages <source-head> <target>` 可预演现代合并策略，但会写对象，只能在隔离副本且授权明确覆盖预判对象写入时使用，不能称为绝对只读。两种方式都要结合本地功能路径和调用链审查；预判不只找 `<<<<<<<`，还要找自动合并但可能改变语义的文件。

冲突候选按以下顺序分组：

1. 文本冲突和同一符号的三方修改。
2. 文件重命名、移动、目录拆分以及旧路径残留。
3. `.style` 生成头、CMake、资源、语言和生成脚本闭环。
4. 子模块 gitlink 与主仓库 API 不一致。
5. 设置、持久化、默认值和迁移兼容性。
6. 版本、品牌、AppId、安装包和 UWP/RC 资源。
7. History/Compose 新旧 UI 路径、选择态、菜单和快捷键。
8. 媒体 streaming、MPV、seek、线程、timer、request 生命周期。
9. CI runner、PCH、MSVC/GCC、Qt、FFmpeg 和缓存键。

### 3.4 大跨度升级与分批分析

默认固定一个最终正式版，在隔离副本的 `merge` 分支完成一次完整合并，分模块审查和适配。Git 会合入目标的祖先历史，无需依次合并每个 Release；按版本号机械拆分可能让同一功能反复迁移和验证。

1. 记录原目录、`source_head`、官方 anchor、目标 tag/SHA、隔离目录、本地 `merge` 分支和各项授权。独立副本从冻结的本地 HEAD 开始，不能直接从官方目标开始而漏掉本地功能。对本仓库优先使用有独立子模块检出的副本，避免多个工作区共用子模块工作树。
2. 比较同一基线下 `Base -> Local` 与 `Base -> Target` 的累计差异，先建立完整清单，再深入交集、重命名/删除、接口调用、设置迁移和生成链。需要解释行为或迁移意图时，再追对应提交历史；无需从头逐条精读全部提交。
3. 按功能及依赖分批：构建/子模块与生成链、公共接口、转发/选择/菜单、History/Compose、streaming/MPV 与媒体展示、翻译/设置、品牌/更新器。属于同一适配的声明、实现、调用、资源和构建清单一起检查。
4. 每批保存模块编号、基线 SHA、相关路径/符号、依赖、三方证据、冲突决策、验证结果和下一步；总索引区分未检查、已分析、待决策、已适配、已验证。继续工作时先核对 HEAD 与记录，从未完成项恢复，不能把已分析当成已验证。
5. 分批完成审查和修复，但每个实际合并节点都包含完整的官方目标和配套依赖。不能按目录拼出部分上游版本，也不能在索引仍有冲突时把各模块当作独立完整合并提交。

只有需要可运行的中间基线，或跨越多轮架构变化导致直接适配难以定位和验证时，才选择少量中间节点。节点应是架构迁移完成、依赖配套的官方提交或标签；每个节点都必须完成静态检查、云构建和受影响功能回归。选择前比较各段风险和重复修改的本地路径，分析分界不必成为实际合并节点。

升级记录放在 `docs/upstream-merges/` 下的独立版本记录或目录中。记录上述字段及第 9 节的交付字段即可；若已有模板可复用，不依赖尚未创建的模板文件。明确只读的阶段不创建这些文件。

## 4. 冲突报告与决策

每个冲突编号都必须有文件路径、符号或行号范围、Base/Local/Upstream 三方证据，并写清：

- 本地功能意图：用户能看到什么、为什么存在、设置键/资源键是什么。
- 上游演进逻辑：从上一次 official anchor 到目标的重命名、拆分、API 或行为变化。
- 直接选 Local 的编译、运行、维护和安全后果。
- 直接选 Upstream 的本地功能损失和回归风险。
- 推荐的重构/适配方案、风险等级、静态检查和回归场景。
- 用户选择、实际执行状态、验证证据和遗留问题。

推荐原则是“保留产品身份和用户功能意图，采用上游新的架构边界”。不得用整文件 ours/theirs 解决版本资源、style 聚合文件或大型 UI 文件；应按符号、字段和资源逐项融合。

版本/品牌的默认政策：保留本地 64Gram 的 `AppId`、`AppName`、`AppFile`、公司/产品字符串和公开产品版本；只更新官方 `UpstreamVersion` 及确实需要的依赖/构建版本。若用户另有版本政策，以当次记录为准。

## 5. 本地功能资产与历史附录

`docs/local-changes.md` 是当前功能的唯一正文，`docs/local-changes-history.md` 是全部本地提交的机器可审计附录；根目录被 `.gitignore` 忽略的 `我的修改.md` 只能作为迁移输入，不能继续作为唯一事实来源。

### 5.1 更新算法

每次升级完成后按以下算法更新：

1. 以新 HEAD 的代码树为事实，先标记当前仍可从入口、设置、资源或行为观察到的功能。
2. 以本地作者身份 `111 <111@example.com>`、`Your Name <you@example.com>`、`C1og <44397460+C1og@users.noreply.github.com>` 为审计范围；排除 merge commit，不把官方 commit 算作本地功能。
3. 对每条本地非 merge 提交记录完整 SHA、日期、标题、主要文件、分类和状态。当前分支审计为 297 条；数字会随提交变化，不能写死为旧文档的 144 条。
4. 使用代码树、`git show`、后续 revert/修复和当前入口复核状态：`active`、`superseded`、`reverted`、`merge-adaptation`、`ci-only`、`docs-only`、`experimental`。
5. 将同一功能线的实验、修复和回退聚合到正文；单独提交仍保留在历史附录，不因聚合而丢失。
6. 标出与上游高耦合的文件、设置键、生成资源、子模块和下一次合并的风险。

### 5.2 当前功能正文最低字段

每项当前功能必须包含：用户可见行为、入口文件/符号、设置键和默认值、资源/语言键、关键本地提交、与上游高耦合区域、回归场景、已知限制和维护注意事项。功能至少覆盖在线播放/MPV/Smart 与 Soft Seek、播放 profiles、翻译后端与缓存/保护、消息菜单与 Details、跨聊天/评论选择与快速复制、Compose 编辑导航、频道发送者标记、emoji/sticker 尺寸、no-forwards 主题、CI/PCH/缓存和版本品牌。

## 6. 历史失败模式与防回归门禁

### 6.1 Style 拆分和生成头可见性

`st::` 定义存在不代表消费方可见。逐个展开 `.style` 的 `using` 闭包，核对直接 include 的生成 style header，并检查 `Telegram/cmake/td_ui.cmake`；同时扫描旧聚合 style 的重复定义。典型失败是 `st::autolockButton` 定义在 `styles/style_passcode_box.h` 对应的 style 中，但使用文件没有直接 include。

### 6.2 重命名、移动和残留旧符号

对上游删除或重命名的声明、定义、调用、设置键、翻译 key、CMake、文档和旧路径做全仓库扫描。示例 `kAlternativeScrollProcessing` 说明只修编译器第一处报错是不够的。

### 6.3 重复定义和新旧入口并存

扫描重复成员、函数、style、资源和类入口；重点检查上游迁移后旧实现是否仍保留。历史中 `Remove duplicate upstream merge definitions` 与 `Fix duplicate profile tabs style definitions` 已是固定回归案例。

### 6.4 Include、PCH 与编译器差异

每个本地改动 `.cpp` 都要审计直接 include，不能依赖 PCH 或间接头“碰巧可见”。检查 MSVC/GCC 模板、宏、重载和可见性差异；PCH 开关、Breakpad/ATL、Qt、FFmpeg、`prepare.py`、runner 和 cache key 变化必须进入报告。Actions 日志只从第一组真实 compiler/linker error 开始分析。

### 6.5 子模块和生成链

对 `Telegram/lib_ui`、`Telegram/lib_base`、`Telegram/lib_crl`、`Telegram/lib_spellcheck`、`Telegram/ThirdParty/MicroTeX`、`Telegram/ThirdParty/TooManyCooks`、`cmake`、`lib_storage`、`tgcalls` 等变化，记录 gitlink 前后 SHA、目标对象远端、工作树 HEAD 和主仓库 API。新增源文件、style、语言、图标、AppX/XML、JSON、Python 生成步骤都必须闭合；不要手工维护 build 目录生成文件。

### 6.6 运行时生命周期与状态机

自动合并成功也要检查 callback、timer、request、widget、线程和资源生命周期；媒体重点检查 seek generation、取消请求、EOF/InvalidData、重复 part、线程 stop/join、MPV bridge 清理和 stale frame timer。History/Compose 重点覆盖 legacy `HistoryWidget` 与新 `HistoryView::ComposeControls/ListWidget` 两条路径。

### 6.7 设置与持久化

简单开关优先 KV prefs。`QDataStream` 新字段只能追加到末尾，读取使用 `!stream.atEnd()` 和明确默认值；重命名设置保留迁移或兼容读取，旧 portable 配置必须能启动。禁止通过插入中间字段破坏旧配置。

### 6.8 编码、脏文件与版本资源

改动文本文件保持仓库约定换行、无 BOM；扫描冲突标记、混合换行、意外日志、截图、`__pycache__` 和构建产物。版本资源逐项核对 `AppId`、`AppName`、`AppFile`、`CompanyName`、`ProductName`、`AppVersion`、`AppVersionStr`、`UpstreamVersion`、`Telegram.rc`、`Updater.rc`、UWP、`setup.iss` 和 changelog。

## 7. 静态验证与云构建

本机没有可依赖的 Telegram C++ 构建环境；不能声称本地编译或运行成功。push 前固定执行并在记录中填结果：

```text
git ls-files -u                         empty
git diff --check                        pass
git diff --cached --check               pass
conflict marker scan                    none
BOM/line-ending scan                    pass
CMake/resource/style audit              pass
Python AST/XML/JSON parse               pass
submodule gitlink/worktree              consistent
version/branding audit                  pass
residual old-symbol scan                pass
duplicate style/definition scan         pass
```

涉及媒体、选择、编辑或 style 拆分时，必须追加对应专项检查。静态检查通过且 commit/push 授权覆盖后，从隔离副本直接将候选提交推送到 `ked33/merge`。推送前重新检查远端 SHA；若 `merge` 出现候选尚未包含的提交，先在隔离副本获取并整合，重新完成受影响的静态检查后再推送。只允许快进更新，不强推覆盖远端历史。

```bash
git push ked33 HEAD:refs/heads/merge
git ls-remote ked33 refs/heads/merge
```

确认返回的远端 SHA 与隔离副本 HEAD 一致后，记录实际推送结果并进入 `ci-pending`。不要求先推远端升级分支或等云构建通过再推进 `merge`。

旧流程已经在本地 `upgrade/*` 分支进行的任务，先从当前候选完成上述推送。远端 SHA 核对成功后，将隔离副本的本地 `merge` 分支快进到该提交并切换过去，再用 `git branch -d <旧升级分支>` 删除已经被 `merge` 包含的本地 `upgrade/*` 分支。逐个核对分支提交及工作树占用，不删除尚未包含的提交，不删除隔离目录，也不顺带删除远端分支。记录清理结果；后续合并直接使用 `merge`。

当前主要云构建入口是 `.github/workflows/build-win-x64.yml` 的 `Build Windows x64`，需要仓库 Secrets `TDESKTOP_API_ID`、`TDESKTOP_API_HASH`；不得打印 secret。先确认工作流会对 `merge` 分支 push 触发，或在获批范围内手动触发。查询 Actions 时必须记录 run URL、workflow、`head_sha`、结论和 artifact。旧 run 或同名 workflow 不代表本次提交已构建。

云编译与运行时结论严格分开：

```text
static: pass/fail
cloud_build: passed/failed/pending/not-run
runtime: verified/user-verified/not-run/blocked/unknown
```

云构建通过后，用该提交的 artifact 完成第 8 节中受影响的回归，包括旧配置和本地定制功能；用户验证同样记录对应提交及结果。这里是对已推送提交的验收，不再安排第二次分支发布。静态、云构建和必需回归全部通过，或用户明确接受列出的验证缺口后，才按第 9 节关闭本轮记录。

云构建或回归发现问题时，在隔离副本修复并检查，继续直接推送到 `ked33/merge`。每个新提交都重新核对远端 SHA 和对应 Actions `head_sha`，不得沿用旧提交的 CI 结论。用户尚未验证的项目保留实际状态，不因已经推送而标记通过。

## 8. 合并后回归矩阵

至少按受影响范围验证：启动、登录、旧设置/portable；翻译后端、LRU、链接/代码保护和本地化；消息菜单、Details、空白区、选择态和 no-forwards；跨聊天/评论拖选、快速复制、selected-copy；Compose 编辑导航和 legacy HistoryWidget；MPV 普通/特殊、首播、seek、暂停、退出、Smart/Soft Seek、FLOOD_PREMIUM_WAIT、EOF、取消请求和线程退出；播放 profiles、emoji/sticker 尺寸、频道发送者；Stories、community、profile、search、媒体查看器；以及上游本次新增或重构功能。

## 9. 单次记录关闭条件与下一次 anchor

只有在目标 tag/SHA、merge-base、范围、所有实质冲突决策、索引无冲突、静态门禁、子模块、push 授权、推送提交 SHA、`ked33/merge` 远端 SHA、Actions `head_sha` 和运行验证都已记录并满足验收条件后，状态才能为 `closed`。推送完成只表示代码已交付；验证缺口必须保留实际状态，只有用户明确接受列出的缺口后才能按例外关闭，不能只写“未验证”就自动关闭。记录末尾写入：

```text
source_head = <本轮冻结的原 checkout HEAD>
official_upstream_anchor = <本次实际合入的官方 parent/目标 SHA>
next_preflight_base = <下一次从哪里计算 merge-base>
integration_branch = merge
push_remote = ked33
push_branch = merge
pushed_candidate_sha = <直接推送到 merge 的候选 SHA>
verified_candidate_sha = <通过静态、云构建及必需回归的候选 SHA；未完成时填 pending>
local_head_after = <隔离副本最终 HEAD>
remote_head_after = <ked33/merge 的远端 SHA>
local_upgrade_branches = <本地旧 upgrade/* 分支清理结果；默认 none>
runtime_status = <真实运行验证状态及已接受的验证缺口>
open_risks = <未关闭风险>
```

若下一次目标仍未 fetch，先用远程只读信息验证并按第 2 节准备隔离副本；不得复用过期 merge-base、旧冲突结论或旧 Actions 结果。长期维护应定期检查官方正式版，按风险安排同步，避免再次积累大跨度差异；这不要求补合每个历史版本。

## 10. 每次升级可复制的执行提示词

```text
## 任务
将本地仓库规划升级到 Telegram Desktop 官方上游：
- 官方仓库：https://github.com/telegramdesktop/tdesktop
- 目标 Release/tag：<vX.Y.Z>
- 目标完整 commit：<full-sha>
- 原 checkout：<source-path>
- 隔离 checkout：<integration-path>
- 本地工作分支：merge
- 推送目标：ked33/merge（直接推送，不先推远端 upgrade 分支）
- 本次记录：docs/upstream-merges/<本次版本记录或目录>

本次范围是最近一次实际合入的 official upstream anchor 到固定目标 commit 的全部官方改动。默认一次完整合并、分模块审查与适配；不逐个历史版本合并，不在过程中追逐新的 dev HEAD。正式合并前必须重新验证 tag、SHA、Release、当前分支、远端、工作区、official parent 和真实 merge-base。

## 硬性门禁
1. 准备授权前只读核查。目标对象缺失时先确认来源及 SHA，列出隔离目录和拟执行的准备操作，再取得缺少的授权；无需等全部冲突决策完成后才获取对象。
2. 准备授权只覆盖指定隔离副本及其 merge 分支、定向 fetch 和所需子模块对象，不覆盖业务源码适配、正式合并或推送；会写预判对象的命令也必须有明确覆盖。
3. 准备/fetch、冲突决策/合并、commit、push 分别记录，可一次授权多项；已有授权时直接继续，不重复询问。push 默认直接更新 ked33/merge，不再安排升级分支推送和长期分支发布两次确认。
4. 正式上游只能使用 telegramdesktop/tdesktop；TDesktop-x64/tdesktop 只能作为参考。
5. 保留原 checkout 和既有脏文件；分析报告只写入已授权记录目录，明确只读时在对话输出。已识别的实质冲突决策齐备后才开始源码合并。

## 准备与预检
1. 记录 source_head、分支、tracking、remote、pushDefault、工作区快照及子模块 gitlink/工作树 HEAD/内部改动。
2. 确认最近一次实际合入的 official parent，不得只按 merge commit 标题猜测。
3. 对象缺失且尚未获准准备时，只用 ls-remote、官方 Release/tag 和完整 diff 流收集事实；获批后从 source_head 建立隔离副本并使用 merge 分支，定向获取目标，再完成本地 Git 三方预检。
4. 对比 Local、Base、Upstream 的累计差异，统计提交、文件、重命名/移动、删除、子模块和生成链变化；路径交集与同一基线位置的修改只能作为候选，不能当作最终冲突数。
5. 对照 docs/local-changes.md，按第 3.4 节分模块审查文本与语义冲突、style/CMake/resource、子模块、设置持久化、版本品牌、History/Compose 双路径、streaming/MPV 生命周期和 CI/PCH 风险。
6. 每批更新总索引、证据、决策、依赖、验证和待办；只有需要可运行的中间基线时才建议中间合并节点，并说明重复适配成本和每个节点的验证计划。

## 每项冲突输出
- 编号、类型、路径、符号或行号范围。
- Base/Local/Upstream 三方证据。
- 本地功能意图和用户可见行为。
- 上游从 official anchor 到目标的演进逻辑。
- 直接保留 Local / 直接接受 Upstream 的具体后果。
- 推荐的重构适配方案、风险等级、静态检查和回归场景。
- 用户选择、执行状态和最终验证字段。

最后汇总已识别冲突及仍需我的决策。支持“全部按推荐方案合并”，但该回复不授权新发现的冲突、commit 或 push。其他授权已经明确给出时不重复索取；缺少必要决策时暂停相关写操作，可以继续独立只读分析。

## 获批后的执行
1. 重新验证固定目标和所有基线，在隔离副本的 merge 分支使用 --no-ff --no-commit 合入完整目标，按批准方案逐项适配；遇到新实质冲突暂停相关适配并补充报告。旧流程已经开始的 upgrade/* 分支保留到推送成功后再清理，不为切换分支重做合并。
2. 主源码、子模块、资源和生成链配套升级。完成本手册规定的静态门禁，全部索引冲突解决后，在 commit 授权范围内提交。
3. 推送前核对 ked33/merge；远端已前进时先在隔离副本整合并重新检查。在 push 授权范围内执行 git push ked33 HEAD:refs/heads/merge，仅允许快进，不强推，不先推远端升级分支。
4. 核对 merge 远端 SHA 与本地 HEAD 一致后，将隔离副本切换到包含该提交的本地 merge 分支，删除已被 merge 包含的本地 upgrade/* 分支。跟踪该提交的 Actions head_sha；云构建或回归失败时修复、静态检查并直接推送修复提交到 merge，重新验证新 SHA，不沿用旧提交的 CI 结论。
5. 使用云构建 artifact 回归受影响功能，更新本次记录和下一次 official anchor。云构建及运行验证在推送后跟进；验证未完成时保留待验证状态，只有验证通过或明确接受列出的缺口后才关闭本轮。

## 验证边界
本机没有可依赖的 C++ 构建环境。必须分别报告：静态检查、GitHub Actions 云编译、运行时验证；不得把其中任何一个冒充另一个。Actions 失败只从第一组真实 compiler/linker error 开始分析。

## 交付文档
按本手册第 3.4、9 节字段建立本次记录，已有模板时可复用；合并完成后同步 docs/local-changes.md 与 docs/local-changes-history.md，写入 official anchor、本地候选与 ked33/merge 的 SHA、Actions、运行验证及下一次合并风险。
```
