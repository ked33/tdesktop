# MPV 分片 MP4 缓存外 seek 修复

`100.mp4` 的缓存外拖动失败同时涉及 HTTP 能力和分片索引。修复恢复有限文件的标准 Range 响应，并允许 FFmpeg 正常发现索引；普通 MP4 已有的文件头修补算法继续使用。

## 根因证据

样本大小为 310,189,901 字节，时长约 844.976 秒，包含 338 个 `moof` 和 338 个 `mdat`，前置 `moov` 内含 `mvex`，没有 `sidx` 或 `mfra` 全局分片索引。

用户提供的 `mpv.log` 第 626 行记录 `Stream is not seekable`。随后，约 651.56 秒缓存范围之外的跳转出现 `Cached seek not possible` 和 `Cannot seek in this stream`；缓存内的 387 秒跳转成功。Telegram 日志只记录从偏移 0 开始的顺序请求。

旧桥接对所有分片 MP4 忽略请求的 Range，返回从文件头开始的 `200 OK`，并省略 `Accept-Ranges`。另外，启动参数对 MP4 一律使用 `fflags=+ignidx`，使无全局索引的分片文件无法获得完整定位信息。

实际 MPV 对照测试发现：

| HTTP / 解复用方式 | `100.mp4` 结果 |
|---|---|
| 旧顺序响应，保留 `+ignidx` | `seekable=false`，760 秒跳转被拒绝 |
| 恢复 Range，仍保留 `+ignidx` | `seekable=true`，但 760 秒跳转在 30 秒后仍处于 seek，持续从前段解码 |
| 恢复 Range，允许正常索引发现 | 760、120、800 秒跳转均完成 |

## 实现

- 普通和特殊 MPV 均依据请求区间返回 `206 Partial Content`、`Content-Range`、正确的内容长度和 `Accept-Ranges: bytes`，读取位置与请求一致。
- 启动选项保留 `ignore_editlist=1`，移除全局 `+ignidx`。FFmpeg 负责解析 `sidx`、`mfra` 和各分片的真实时间、采样信息。
- 保留普通前置 `moov` 文件的首个 `mdat` 长度修补。它让普通多数据块 MP4 在正常索引发现模式下仍能快速打开和定位；只修改输出缓冲，采样偏移不变。
- 分片 MP4 不应用上述 `mdat` 修补，其 `moof` 和 `mdat` 边界完整保留。
- 大文件头和分片 MP4 的非零 Range 使用独立 Reader。请求代次在获取读取锁之前更新，旧请求可在等数据时被新请求替代。
- 特殊 MPV 复用已有的可取消读取循环，通知对象随 Entry 存活；结束废弃读取时先在主线程清理通知和读取需求，再释放读取锁。
- 断连后结束对应读取，不再继续后台顺序扫描；普通 Range 读取块统一为 64 KiB。

## 回归验证

2026-09-12 使用本机 MPV、空视频/音频输出、本地 HTTP 服务和 `--cache=no` 验证。测试从真实启动代码提取解复用选项，并通过编译后的 `test_mp4_header --inspect` 获取生产代码生成的文件头修补。

| 样本 | 单次跳转 | 快速连续跳转 |
|---|---:|---:|
| 普通前置 `moov` | 5/5 | 通过 |
| 3 MiB 大 `moov` | 5/5 | 通过 |
| 多 `mdat` | 5/5 | 通过 |
| 大 `moov` 与多 `mdat` 组合 | 5/5 | 通过 |
| 尾部 `moov` | 5/5 | 通过 |
| 含 `mfra` 的分片 MP4 | 5/5 | 通过 |
| 含 `sidx` 的分片 MP4 | 5/5 | 通过 |
| 无全局索引的生成分片 MP4 | 5/5 | 通过 |
| 用户样本 `100.mp4` | 5/5 | 通过 |

合计 45 次单次跳转和 9 轮连续跳转通过；每轮连续发送 6 个跳转命令，检查最终位置完成定位。单次测试检查实际播放位置、seek 状态，并确认首次远距离跳转确实产生新的 HTTP Range 请求。

`100.mp4` 最终测试跳到约 760、127、803、338、718 秒，每次约 0.10–0.18 秒；连续跳转最终定位约 0.22 秒。以上均为本机 HTTP 结果，不代表 Telegram 远程下载速度。

另外，通过 MSVC 独立 Debug 编译并执行了 MP4 文件头单测（42,883 项检查）和 MPV 可取消读取单测（15 项检查）；Smart 策略公式及 56 项结构检查、定向 diff 检查均通过。

## 首播成本与验证边界

无全局分片索引的文件必须先读取分片元数据才能可靠按时间定位。`100.mp4` 的最终测试在启动阶段产生 337 次 HTTP 请求，本地约 2.20 秒；底层按块读取和预读会同时带入部分媒体数据。远程网络下，这一步可能更慢，不能把本次修复描述成所有文件都可立即首播、立即任意跳转。

本次保留并验证了此前大 `moov`、多 `mdat` 的成功路径，但没有拿到此前所有真实视频。新增回归覆盖这些布局及其组合，不能代替全部实际媒体的验证。

测试中的 HTTP 服务用于验证 MPV 解复用和文件头兼容性，没有运行 Telegram 的真实 Reader/MTProto 链路。当前工作区缺少配置好的 `out` 和项目依赖目录，因此未进行 Telegram 整包编译或真实远程播放验证。

## 复现命令

先获得 Debug 构建的 `test_mp4_header`，然后从仓库根目录运行：

```bash
python Telegram/SourceFiles/test/test_mpv_seek.py \
  --mpv "D:/D-Software/mpv-hero/mpv.exe" \
  --ffmpeg "D:/D-Software/yt-dlp/ffmpeg.exe" \
  --header-test "out/Debug/test_mp4_header.exe" \
  --sample "D:/D-Download/100.mp4"
```

脚本自动生成回归媒体并把日志、请求记录和 `summary.json` 写到独立临时目录；不传 `--sample` 时也能验证生成样本。
