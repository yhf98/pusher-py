# pusher SDK API 接口说明

本文档说明 `pusher` Python SDK、CLI 参数、默认值、运行环境变量和构建相关变量。当前插件包名为 `pusher`，根据输出 URL 协议自动选择 native worker：RTMP/RTSP/SRT/RTP 使用 FFmpeg SDK 处理，WHIP 使用内嵌 WHIP/WebRTC worker。文件/网络流默认 remux/copy；摄像头输入会 native 采集并编码为 H264。所有运行时推流路径都在 Python 进程内执行，不启动外部推流程序。

## 快速导入

```python
from pusher import Pusher, build_output_url, detect_protocol, version
```

源码目录运行时需要指定：

```bash
PYTHONPATH=/root/workspace/ms-fish-recg-pro/pusher-py/src python test-push.py
```

## Pusher 构造函数

```python
pusher = Pusher(
    name="default",
    timeout_ms=5000,
    auto_reconnect=True,
    log_path="",
    loop=True,
    realtime=True,
    width=1280,
    height=720,
    fps=30,
    bitrate=2000000,
    analyzeduration_us=10000000,
    probesize=50000000,
)
```

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `name` | `str` | `"default"` | 推流实例名称，用于 `status()` 输出；不能为空。 |
| `timeout_ms` | `int` | `5000` | 网络打开和停止等待的超时时间，单位毫秒；必须大于 0。 |
| `auto_reconnect` | `bool` | `True` | 预留参数；当前 native `libav` 路径尚未实现自动重连。 |
| `log_path` | `str` | `""` | 日志文件路径；为空时不写 SDK 日志。 |
| `loop` | `bool` | `True` | 本地文件输入读到 EOF 后是否循环；网络流输入不循环。 |
| `realtime` | `bool` | `True` | 本地文件输入是否按媒体时间戳限速；网络流输入不需要限速。 |
| `width` | `int` | `1280` | 摄像头采集和编码目标宽度；必须大于 0。 |
| `height` | `int` | `720` | 摄像头采集和编码目标高度；必须大于 0。 |
| `fps` | `int` | `30` | 摄像头帧率；也用于缺失视频时间戳时估算帧间隔。 |
| `bitrate` | `int` | `2000000` | 摄像头 H264 编码目标码率，单位 bit/s；必须大于 0。 |
| `analyzeduration_us` | `int` | `10000000` | RTSP/网络流探测时长，单位微秒。 |
| `probesize` | `int` | `50000000` | RTSP/网络流探测数据量，单位字节。 |

兼容参数：

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `engine` | `str` | `"auto"` | 兼容/调试参数，业务代码通常不要传。`auto` 会按输出 URL 自动选择 worker；`ffmpeg` 已禁用。 |

### 自动协议路由

| 输出协议 | 自动处理方式 |
| --- | --- |
| `rtmp` / `rtmps` | FFmpeg SDK remux/copy worker，输出封装为 `flv`。 |
| `rtsp` / `rtsps` | FFmpeg SDK remux/copy worker，输出封装为 `rtsp`。 |
| `srt` | FFmpeg SDK remux/copy worker，输出封装为 `mpegts`；要求当前 SDK 编译时启用 SRT。 |
| `rtp` | FFmpeg SDK remux/copy worker，输出封装为 `rtp`。 |
| `whip` | 内嵌 WHIP/WebRTC worker，不执行 `whip-push-demo` 或 `stream_push` 可执行文件。 |

## Pusher 方法

### start(input, output_url)

启动推流任务。

```python
pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)
```

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `input` | `str` | 是 | 输入地址。支持本地文件、`rtsp://`、`rtmp://`、`http://`、`https://`、`srt://`、Linux `/dev/video*` 摄像头，以及 Windows dshow `video=设备名`。 |
| `output_url` | `str` | 是 | 输出地址。根据 URL 自动识别协议。支持 `rtmp://`、`rtsp://`、`srt://`、`rtp://`、WHIP HTTP URL。 |

返回值：`None`。

异常：

- `ValueError`：参数为空、协议不支持、本地文件不存在、配置值非法。
- `RuntimeError`：已在运行或 native worker 启动失败。

注意：`start()` 启动后台任务后立即返回，业务进程必须保持运行；脚本退出会析构 `Pusher` 并停止推流。

### stop(timeout_ms=-1)

停止当前推流任务。

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `timeout_ms` | `int` | `-1` | 停止等待时间。`-1` 表示使用构造函数的 `timeout_ms`。 |

返回值：`None`。native worker 会收到停止请求并退出。

### wait(timeout_ms=-1)

等待推流任务结束。

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `timeout_ms` | `int` | `-1` | 等待时间，单位毫秒。负数表示一直等待。 |

返回值：

- `int`：任务结束时返回退出码，`0` 表示正常退出。
- `None`：等待超时但任务仍在运行。

### status()

返回当前任务状态字符串。

```python
print(pusher.status())
```

常见字段：

| 字段 | 说明 |
| --- | --- |
| `name` | 实例名称。 |
| `state` | `running` 或 `stopped`。 |
| `worker=thread` | 表示当前任务是 C++ worker 线程。 |
| `pid` | 始终为 `-1`；运行时不启动外部推流进程。 |
| `exit_code` | 任务结束后的退出码。 |
| `error` | native worker 执行错误。 |
| `engine` | 内部 worker 名称，用于调试。 |
| `protocol` | 输出协议。 |
| `input` | 输入地址。 |
| `output_url` | 输出地址。 |
| `command` | 预览形式的 native 任务描述。 |

### preview_command(input, output_url)

返回将要执行的命令/任务描述，不启动推流。

```python
cmd = pusher.preview_command("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
print(" ".join(cmd))
```

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `input` | `str` | 是 | 输入地址或本地文件路径。 |
| `output_url` | `str` | 是 | 输出地址。 |

返回值：`list[str]`。`libav` 模式返回类似 `["libav-remux", input, "->", output_url, "protocol=rtmp", ...]`。

### command()

返回当前已经启动任务的命令/任务描述。

返回值：`list[str]`。未启动时返回空列表。

### 上下文管理器

`Pusher` 支持 `with` 语法，退出上下文时自动调用 `stop()`。

```python
with Pusher(loop=False, realtime=False) as p:
    p.start(input_url, output_url)
    p.wait(timeout_ms=10000)
```

## Pusher 属性

| 属性 | 类型 | 说明 |
| --- | --- | --- |
| `is_running` | `bool` | 当前任务是否仍在运行；读取时会顺便回收已退出的任务状态。 |
| `protocol` | `str` | `start()` 后识别到的输出协议；未启动时为空字符串。 |
| `engine` | `str` | 内部 worker 名称，用于调试；正常业务按 `output_url` 自动路由。 |
| `name` | `str` | 实例名称。 |
| `pid` | `int` | 始终为 `-1`；native 推流使用 worker 线程。 |
| `exit_code` | `int | None` | 任务退出码；运行中或未产生退出码时为 `None`。 |

## 模块级函数

### version()

返回 native 扩展版本。

```python
assert version() == "0.1.9"
```

返回值：`str`。

### detect_protocol(url)

根据输出 URL 识别协议。

| 参数 | 类型 | 说明 |
| --- | --- | --- |
| `url` | `str` | 输出 URL。 |

返回值：

| URL 示例 | 返回值 |
| --- | --- |
| `rtmp://127.0.0.1/live/test` | `rtmp` |
| `rtsp://127.0.0.1/live/test` | `rtsp` |
| `srt://127.0.0.1:10080` | `srt` |
| `rtp://127.0.0.1:5004` | `rtp` |
| `http://host/rtc/v1/whip/` | `whip` |
| `file:///tmp/a.mp4` | `unknown` |

### build_output_url(protocol, host, app="live", stream="test", secret=None, port=0, use_tls=False)

构造常见推流输出地址。

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `protocol` | `str` | 必填 | `rtmp`、`rtsp`、`srt`、`rtp`、`whip`。 |
| `host` | `str` | 必填 | 目标服务器 IP 或域名；不能为空。 |
| `app` | `str` | `"live"` | 应用名，例如 RTMP 的 `/live/`。 |
| `stream` | `str` | `"test"` | 流名，例如 `detect_1500`。 |
| `secret` | `str | None` | `None` | 鉴权参数；非空时追加到 URL 查询参数。 |
| `port` | `int` | `0` | 端口。`0` 表示按协议使用默认端口。 |
| `use_tls` | `bool` | `False` | 是否使用 TLS，例如 `rtmps`、`rtsps` 或 HTTPS WHIP。 |

示例：

```python
url = build_output_url(
    protocol="rtmp",
    host="192.168.0.138",
    app="live",
    stream="test",
    secret="557ea19cf905454bad9dc988d0c6a5g1",
)
# rtmp://192.168.0.138:1935/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1
```

## 输出协议映射

| 输出协议 | native 处理方式 | 说明 |
| --- | --- | --- |
| `rtmp` / `rtmps` | FFmpeg SDK remux，封装 `flv` | 常用直播推流格式。 |
| `rtsp` / `rtsps` | FFmpeg SDK remux，封装 `rtsp` | 默认输出侧设置 `rtsp_transport=tcp`。 |
| `srt` | FFmpeg SDK remux，封装 `mpegts` | 要求 FFmpeg SDK 编译时支持 SRT 协议。 |
| `rtp` | FFmpeg SDK remux，封装 `rtp` | 单路 RTP 输出。 |
| `whip` | 内嵌 WHIP/WebRTC worker | 不走 FFmpeg muxer，不启动外部程序。 |

## CLI 命令

安装后可使用两个入口，功能相同：

```bash
pusher --help
pusher-py --help
```

源码目录运行：

```bash
PYTHONPATH=src python -m pusher.cli --help
```

### pusher push

启动真实推流。

```bash
pusher push INPUT OUTPUT_URL --status-interval 5
```

位置参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `input` | 是 | 输入地址或本地文件。 |
| `output_url` | 是 | 输出推流地址。 |

专用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--name` | `cli` | 实例名称。 |
| `--status-interval` | `0.0` | 状态打印间隔，单位秒；`0` 表示不定期打印，只等待任务结束。 |

通用参数见“CLI 通用参数”。

### pusher preview

打印任务描述，不启动推流。

```bash
pusher preview INPUT OUTPUT_URL --json
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `input` | 必填 | 输入地址或本地文件。 |
| `output_url` | 必填 | 输出推流地址。 |
| `--name` | `preview` | 实例名称。 |
| `--json` | `False` | 以 JSON 数组输出命令参数。 |

### pusher build-url

构造输出 URL。

```bash
pusher build-url rtmp 192.168.0.138 --app live --stream test --secret xxx
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `protocol` | 必填 | `rtmp`、`rtsp`、`srt`、`rtp`、`whip`。 |
| `host` | 必填 | 目标服务器 IP 或域名。 |
| `--app` | `live` | 应用名。 |
| `--stream` | `test` | 流名。 |
| `--secret` | `""` | 鉴权 secret，非空时追加到 URL。 |
| `--port` | `0` | 端口；`0` 表示使用协议默认值。 |
| `--tls` | `False` | 使用 TLS 地址。 |

### pusher detect

识别输出 URL 协议。

```bash
pusher detect rtmp://192.168.0.138:1935/live/test
```

| 参数 | 说明 |
| --- | --- |
| `url` | 需要识别的 URL。 |

### CLI 通用参数

`push` 和 `preview` 都支持以下参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--log-path` | `""` | 日志文件路径。 |
| `--timeout-ms` | `5000` | 超时时间，单位毫秒。 |
| `--width` | `1280` | 摄像头采集和编码目标宽度。 |
| `--height` | `720` | 摄像头采集和编码目标高度。 |
| `--fps` | `30` | 摄像头帧率；也用于缺失时间戳时估算帧间隔。 |
| `--bitrate` | `2000000` | 摄像头 H264 编码目标码率，单位 bit/s。 |
| `--analyzeduration-us` | `10000000` | 网络流探测时长，单位微秒。 |
| `--probesize` | `50000000` | 网络流探测数据量，单位字节。 |
| `--no-loop` | `False` | 设置后关闭本地文件循环，相当于 `loop=False`。 |
| `--no-realtime` | `False` | 设置后关闭本地文件实时限速，相当于 `realtime=False`。 |

## 系统环境变量

| 环境变量 | 是否必需 | 示例 | 说明 |
| --- | --- | --- | --- |
| `PYTHONPATH` | 源码运行必需 | `PYTHONPATH=pusher-py/src` | 未安装包时，让 Python 找到 `pusher` 模块。开发安装 `pip install -e .` 后可不设置。 |
| `LD_LIBRARY_PATH` | 通常不需要 | `LD_LIBRARY_PATH=pusher-py/lib:$LD_LIBRARY_PATH` | 扩展已写入 rpath，默认会加载插件目录 `lib/`；手工移动 `.so` 或排查动态库时可设置。 |
| `PATH` | 构建/CLI 按需 | `PATH=/opt/bin:$PATH` | `pusher` CLI、`readelf`、`make` 等构建或开发命令查找路径。运行时推流不查找外部 `ffmpeg` 或 `stream_push`。 |
| `CC` / `CXX` | 构建时可选 | `CXX=g++` | 指定 C/C++ 编译器。 |
| `CFLAGS` / `CXXFLAGS` | 构建时可选 | `CXXFLAGS="-O2"` | 标准 Python 扩展构建参数；项目会额外追加本地 `include/` 和 C++17。 |
| `LDFLAGS` | 构建时可选 | `LDFLAGS="-Wl,--as-needed"` | 标准链接参数；项目会额外链接本地 `libavformat`、`libavcodec`、`libavutil`。 |
| `PYTHONPYCACHEPREFIX` | 可选 | `PYTHONPYCACHEPREFIX=/tmp/pycache` | 源码目录不可写或不希望生成 `__pycache__` 时使用。 |
| `PUSH_SECONDS` | `test-push.py` 可选 | `PUSH_SECONDS=10` | 项目根目录测试脚本的运行秒数；`0` 表示一直运行。 |
| `PUSH_STATUS_INTERVAL` | `test-push.py` 可选 | `PUSH_STATUS_INTERVAL=1` | 项目根目录测试脚本状态打印间隔，单位秒。 |

## 构建默认目录变量

`setup.py` 和 `scripts/build_ffmpeg.sh` 使用插件目录内的 SDK，不读取 `/usr/local/ffmpeg`。PyPI 发布包不包含 `third_party/FFmpeg` 全量源码，也不包含本机编译出的 `lib/*.so`；从源码安装时需要提前准备本地 FFmpeg SDK，或从 GitHub 仓库获取完整源码后构建。GitHub Actions 发布 Linux wheel 时会从仓库内 `third_party/FFmpeg` 自动构建 SDK，并在 wheel repair 阶段携带运行所需的 FFmpeg `.so` 动态库。

| 变量/目录 | 默认值 | 说明 |
| --- | --- | --- |
| `ROOT` | `pusher-py/` | 插件根目录。 |
| `LOCAL_INCLUDE` | `pusher-py/include` | 本地 FFmpeg SDK 头文件和 pusher 头文件。 |
| `LOCAL_LIB` | `pusher-py/lib` | 本地 FFmpeg 动态库目录。 |
| `SRC_DIR` | `pusher-py/third_party/FFmpeg` | 内置 FFmpeg 源码目录。 |
| `BUILD_DIR` | `pusher-py/build/ffmpeg` | FFmpeg 自动编译临时目录。 |
| `PREFIX` | `pusher-py/.ffmpeg-prefix` | FFmpeg 自动编译安装前缀，再复制到 `include/` 和 `lib/`。 |

## 平台发布范围

当前 CI 自动构建 Linux `x86_64`、`aarch64`、`armv7l` wheel，以及 Windows `x86`、`x64`、`ARM64` wheel。Linux 使用 `scripts/build_ffmpeg.sh` 构建 `.so` SDK；Windows 使用 MSVC + MSYS2 执行 `scripts/build_ffmpeg.ps1` 构建 `.lib`/`.dll` SDK。RK 系列、树莓派、香橙派等开发板通常对应 `aarch64` 64 位系统或 `armv7l` 32 位系统；通用 wheel 只覆盖 CPU/系统 ABI，不包含板载硬编解码栈。

构建命令：

```bash
cd /root/workspace/ms-fish-recg-pro/pusher-py
python setup.py build_ext --inplace --force
```

如果 `lib/libavformat.so` 不存在，`setup.py` 会自动执行：

```bash
scripts/build_ffmpeg.sh
```

## 推荐 RTSP 到 RTMP 示例

```python
import time
from pusher import Pusher

p = Pusher(
    loop=False,
    realtime=False,
    timeout_ms=5000,
    analyzeduration_us=10000000,
    probesize=50000000,
    log_path="pusher.log",
)

p.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)

try:
    while p.is_running:
        print(p.status())
        time.sleep(5)
finally:
    p.stop(3000)
```

短时测试：

```bash
cd /root/workspace/ms-fish-recg-pro
PUSH_SECONDS=10 PUSH_STATUS_INTERVAL=1 PYTHONPATH=pusher-py/src python test-push.py
```

## 当前能力边界

- 文件和网络流默认 copy/remux 转推；摄像头输入会 native 采集并编码为 H264。
- 运行时不启动外部 `ffmpeg` 或 `stream_push` 程序；`engine="ffmpeg"` 已禁用。
- Linux `/dev/video*` 和 Windows `video=设备名` 摄像头输入走 native 采集和 H264 编码；FFmpeg SDK 必须包含 `avdevice`、`swscale` 和可用 H264 编码器。
- `auto_reconnect` 是预留字段，当前断流后不会自动重连。
- 复杂音视频转码、滤镜、水印、AI 处理后编码输出，需要后续扩展 libav 解码/编码路径。
