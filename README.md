# pusher

`pusher` 是一个基于 FFmpeg SDK 二次开发的 Python C/C++ 原生转推插件。默认引擎是 `libav`，直接链接本项目内的 `include/` 与 `lib/`，在当前 Python 进程内启动 C++ worker 线程完成转推，不需要为每路流启动一个 `ffmpeg` 命令进程。

完整 SDK 方法、参数、默认值、环境变量和 CLI 说明见 [SDK_API.md](SDK_API.md)。

它适合 `-c copy` 这类复制转推场景，例如：

```bash
ffmpeg -rtsp_transport tcp -i rtsp://192.168.0.206:8554/av0_0 \
  -c copy -f flv rtmp://192.168.0.138:1935/live/detect_1500
```

在本项目中对应为：

```python
from pusher import Pusher

pusher = Pusher(engine="libav", loop=False, realtime=False)
pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)
```

## 引擎说明

- `libav`：默认引擎。使用 FFmpeg SDK 在进程内 remux 转推，适合 RTSP/MP4/HTTP/SRT 输入转 RTMP/RTSP/SRT/RTP 输出。
- `ffmpeg`：兼容模式。启动外部 `ffmpeg` 命令进程，便于对比排错。
- `stream_push`：外部 C++ 程序模式，主要用于 WHIP/WebRTC。

100 路复制转推时建议使用 `libav`。每路流是一个 C++ worker 线程，不会产生 100 个 `ffmpeg` 子进程。

## 目录结构

```text
pusher-py/
├── include/
│   ├── pusher/              # 本项目 C++ 头文件
│   ├── libavformat/         # FFmpeg SDK 头文件
│   ├── libavcodec/
│   └── libavutil/
├── src/pusher/
│   ├── _native.cpp          # CPython C API 绑定层
│   ├── pusher.cpp           # libav 转推和进程兼容模式
│   ├── url_utils.c          # 协议识别
│   ├── cli.py               # 命令行工具
│   └── __init__.py
├── examples/basic_usage.py
├── lib/                     # FFmpeg 动态库
├── scripts/build_ffmpeg.sh  # 从源码自动编译 FFmpeg SDK
├── third_party/FFmpeg/      # 内置 FFmpeg 源码
├── tests/test_basic.py
├── pyproject.toml
└── setup.py
```

## SDK 依赖

开发仓库内可放置 FFmpeg SDK 文件：

```text
include/libavformat
include/libavcodec
include/libavutil
lib/libavformat.so
lib/libavcodec.so
lib/libavutil.so
third_party/FFmpeg
```

PyPI 发布包不包含 `third_party/FFmpeg` 全量源码，也不包含本机编译出的 `lib/*.so` 构建产物。源码安装时需要提前准备本地 FFmpeg SDK，或从 GitHub 仓库获取完整源码后执行构建。

构建顺序：

1. 如果 `lib/libavformat.so` 已存在，直接使用本地动态库。
2. 如果本地动态库不存在，`setup.py` 会调用 `scripts/build_ffmpeg.sh`，从 `third_party/FFmpeg` 自动编译共享库并安装到本项目 `include/`、`lib/`。

构建脚本不会读取 `/usr/local/ffmpeg`，也不要求用户提前安装或编译 FFmpeg。

自动编译采用最小 FFmpeg SDK 配置，主要启用 `avformat`、`avcodec`、`avutil` 以及 RTSP/RTMP/RTP/HTTP/TCP/UDP/文件协议和常见 muxer/demuxer。目标是 copy/remux 转推，不包含 ffmpeg/ffprobe 命令行程序。

## CI 发布平台

GitHub Actions 会从仓库内的 `third_party/FFmpeg` 自动构建 FFmpeg SDK，再构建 Python wheel。PyPI wheel 不包含 `third_party/FFmpeg` 源码；Linux wheel 会携带运行所需的 FFmpeg `.so` 动态库。

当前自动发布目标是 Linux `x86_64`、`aarch64`、`armv7l`，以及 Windows `x86`、`x64`、`ARM64`。Linux 使用 `scripts/build_ffmpeg.sh`，Windows 使用 MSVC + MSYS2 执行 `scripts/build_ffmpeg.ps1` 并产出 `.lib`/`.dll` SDK。RK 系列、树莓派、香橙派等开发板通常使用 `aarch64` 64 位系统或 `armv7l` 32 位系统；板载硬编解码能力需要单独的设备镜像/系统库适配，不包含在通用 PyPI wheel 中。

## 安装与构建

```bash
cd /root/workspace/ms-fish-recg-pro/pusher-py
python -m pip install -U pip setuptools wheel
python setup.py build_ext --inplace --force
```

开发安装：

```bash
python -m pip install -e ".[dev]"
```

验证导入：

```bash
PYTHONPATH=src python -c "from pusher import Pusher; print(Pusher)"
```

## Python 使用示例

### RTSP 转 RTMP

```python
import time

from pusher import Pusher

pusher = Pusher(
    engine="libav",
    log_path="pusher.log",
    loop=False,
    realtime=False,
    timeout_ms=5000,
)

pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)

print(pusher.status())

while pusher.is_running:
    time.sleep(5)
    print(pusher.status())
```

注意：`start()` 会启动后台 C++ worker 线程并立即返回，业务进程需要保持运行。脚本如果打印一次状态后直接退出，`Pusher` 对象析构会停止转推。

### RTSP 转 RTMP 带鉴权地址

```python
import time

from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="rtmp",
    host="192.168.0.138",
    app="live",
    stream="test",
    secret="557ea19cf905454bad9dc988d0c6a5g1",
)

pusher = Pusher(engine="libav", loop=False, realtime=False)
pusher.start("rtsp://192.168.0.206:8554/av0_0", output_url)

try:
    while pusher.is_running:
        print(pusher.status())
        time.sleep(5)
finally:
    pusher.stop(3000)
```

生成的地址：

```text
rtmp://192.168.0.138:1935/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1
```

### 本地 MP4 循环推 RTMP

```python
from pusher import Pusher

pusher = Pusher(engine="libav", loop=True, realtime=True)
pusher.start("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
```

### 本地 MP4 单次推 RTMP

```python
from pusher import Pusher

pusher = Pusher(engine="libav", loop=False, realtime=True)
pusher.start("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
exit_code = pusher.wait(timeout_ms=-1)
print("exit:", exit_code)
```

### RTSP 转 RTSP

```python
from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="rtsp",
    host="192.168.0.138",
    app="live",
    stream="detect_1500",
    port=8554,
)

pusher = Pusher(engine="libav", loop=False, realtime=False)
pusher.start("rtsp://192.168.0.206:8554/av0_0", output_url)
print(pusher.status())
```

### RTSP 转 SRT

```python
from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="srt",
    host="192.168.0.138",
    app="live",
    stream="detect_1500",
    port=10080,
)

pusher = Pusher(engine="libav", loop=False, realtime=False)
pusher.start("rtsp://192.168.0.206:8554/av0_0", output_url)
print(pusher.status())
```

SRT 需要当前 FFmpeg SDK 编译时启用 `srt` 协议；默认脚本未启用外部 `libsrt` 依赖。

### RTSP 转 RTP

```python
from pusher import Pusher

pusher = Pusher(engine="libav", loop=False, realtime=False)
pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtp://192.168.0.138:5004",
)
print(pusher.status())
```

### MP4 推 WHIP/WebRTC

WHIP 不走 FFmpeg SDK muxer，必须使用 `stream_push` 引擎。`stream_push_path` 需要指向已经编译好的 `stream_push` 可执行文件，或确保它在 `PATH` 中。

```python
import time

from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="whip",
    host="192.168.0.138",
    app="live",
    stream="test",
    secret="replace-me",
    port=1985,
)

pusher = Pusher(
    engine="stream_push",
    stream_push_path="/root/workspace/ms-fish-recg-pro/whip-push-demo/build/stream_push",
    loop=True,
    bitrate=2_000_000,
)

pusher.start("sample.mp4", output_url)

try:
    while pusher.is_running:
        print(pusher.status())
        time.sleep(5)
finally:
    pusher.stop(3000)
```

生成的 WHIP 地址格式：

```text
http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test&secret=replace-me
```

### 摄像头推 WHIP/WebRTC

```python
from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="whip",
    host="192.168.0.138",
    app="live",
    stream="camera0",
    port=1985,
)

pusher = Pusher(
    engine="stream_push",
    stream_push_path="/root/workspace/ms-fish-recg-pro/whip-push-demo/build/stream_push",
    width=1280,
    height=720,
    fps=30,
    bitrate=2_000_000,
)

pusher.start("/dev/video0", output_url)
print(pusher.status())
```

### 使用 ffmpeg 兼容模式

```python
from pusher import Pusher

pusher = Pusher(
    engine="ffmpeg",
    ffmpeg_path="ffmpeg",
    loop=False,
    realtime=False,
)

pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)
print(pusher.status())
```

`ffmpeg` 模式会启动外部进程，只建议用于对比排错、摄像头采集或需要转码的临时场景。

### 停止与等待

```python
exit_code = pusher.wait(timeout_ms=10_000)
if exit_code is None:
    pusher.stop(timeout_ms=3000)
```

### 多路转推示例

```python
from pusher import Pusher

tasks = []
for i in range(100):
    p = Pusher(name=f"stream-{i}", engine="libav", loop=False, realtime=False)
    p.start(
        f"rtsp://192.168.0.{100 + i}:8554/av0_0",
        f"rtmp://192.168.0.138:1935/live/detect_{i}",
    )
    tasks.append(p)

try:
    for p in tasks:
        print(p.status())
finally:
    for p in tasks:
        p.stop()
```

### 协议识别和 URL 构造

```python
from pusher import build_output_url, detect_protocol, version

print(version())
print(detect_protocol("rtmp://192.168.0.138:1935/live/test"))

for protocol in ["rtmp", "rtsp", "srt", "rtp", "whip"]:
    print(protocol, build_output_url(protocol, "192.168.0.138", stream="demo"))
```

## 命令行工具

### RTSP 转 RTMP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1500 \
  --engine libav \
  --no-loop \
  --no-realtime \
  --log-path pusher.log \
  --status-interval 5
```

### RTSP 转 RTMP 预览

```bash
PYTHONPATH=src python -m pusher.cli preview \
  rtsp://192.168.0.206:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1500 \
  --engine libav \
  --no-loop \
  --no-realtime
```

### 本地 MP4 推 RTMP

```bash
PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  rtmp://127.0.0.1:1935/live/test \
  --engine libav \
  --status-interval 5
```

### RTSP 转 RTSP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtsp://192.168.0.138:8554/live/detect_1500 \
  --engine libav \
  --no-loop \
  --no-realtime
```

### RTSP 转 SRT

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  'srt://192.168.0.138:10080?streamid=live/detect_1500' \
  --engine libav \
  --no-loop \
  --no-realtime
```

### RTSP 转 RTP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtp://192.168.0.138:5004 \
  --engine libav \
  --no-loop \
  --no-realtime
```

### MP4 推 WHIP/WebRTC

```bash
PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test&secret=replace-me' \
  --engine stream_push \
  --stream-push-path /root/workspace/ms-fish-recg-pro/whip-push-demo/build/stream_push \
  --status-interval 5
```

### 摄像头推 WHIP/WebRTC

```bash
PYTHONPATH=src python -m pusher.cli push \
  /dev/video0 \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=camera0' \
  --engine stream_push \
  --stream-push-path /root/workspace/ms-fish-recg-pro/whip-push-demo/build/stream_push \
  --width 1280 \
  --height 720 \
  --fps 30 \
  --bitrate 2000000 \
  --status-interval 5
```

### ffmpeg 兼容模式

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1500 \
  --engine ffmpeg \
  --ffmpeg-path ffmpeg \
  --no-loop \
  --no-realtime
```

### 构造和识别 URL

```bash
PYTHONPATH=src python -m pusher.cli build-url rtmp 192.168.0.138 \
  --app live \
  --stream test \
  --secret 557ea19cf905454bad9dc988d0c6a5g1

PYTHONPATH=src python -m pusher.cli build-url whip 192.168.0.138 \
  --app live \
  --stream test \
  --secret replace-me \
  --port 1985

PYTHONPATH=src python -m pusher.cli detect \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test'
```

安装后也可使用：

```bash
pusher push input output_url --engine libav
pusher preview input output_url --json
pusher build-url rtmp 192.168.0.138 --stream demo
```

## 支持协议

输出协议映射：

| 输出 URL | 推荐引擎 | 输出封装/程序 |
| --- | --- | --- |
| `rtmp://` / `rtmps://` | `libav` | `flv` |
| `rtsp://` / `rtsps://` | `libav` | `rtsp` |
| `srt://` | `libav` | `mpegts`，要求当前 FFmpeg SDK 编译时启用 `srt` 协议 |
| `rtp://` | `libav` | `rtp` |
| `http://.../rtc/v1/whip/` / `https://.../rtc/v1/whip/` | `stream_push` | WHIP/WebRTC |

WHIP/WebRTC 不属于 FFmpeg 常规 muxer 输出，本项目使用 `stream_push` 外部引擎处理。

## 参数说明

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `engine` | `auto` | `auto` 会对非 WHIP 输出选择 `libav` |
| `log_path` | 空 | libav 运行日志 |
| `loop` | `True` | 本地文件读到 EOF 后是否循环 |
| `realtime` | `True` | 本地文件是否按媒体时间戳限速 |
| `timeout_ms` | `5000` | 网络打开/停止等待超时 |
| `analyzeduration_us` | `10000000` | RTSP/网络流探测时长，单位微秒 |
| `probesize` | `50000000` | RTSP/网络流探测数据量，摄像头首包缺少尺寸时可增大 |
| `ffmpeg_path` | `ffmpeg` | 仅 `engine="ffmpeg"` 使用 |
| `stream_push_path` | `stream_push` | 仅 `engine="stream_push"` 使用 |

## 测试

```bash
PYTHONPATH=src python -m pusher.cli preview sample.mp4 rtmp://127.0.0.1/live/test
PYTHONPATH=src python examples/basic_usage.py
```

项目根目录的 `test-push.py` 可用于真实 RTSP 到 RTMP 验证：

```bash
cd /root/workspace/ms-fish-recg-pro
PYTHONPATH=pusher-py/src python test-push.py
PUSH_SECONDS=10 PUSH_STATUS_INTERVAL=1 PYTHONPATH=pusher-py/src python test-push.py
```

安装开发依赖后：

```bash
python -m pytest -q
```

## 注意事项

- `libav` 当前实现是 remux/copy 转推，不做解码、滤镜和重编码。
- 如果输入编码或目标协议不被当前 FFmpeg SDK 支持，任务会退出并在 `status()` 或日志中给出错误。
- 自动编译脚本默认不启用外部 `libsrt` 依赖；如需 SRT，请在系统安装 libsrt 开发包后调整 `scripts/build_ffmpeg.sh` 的 configure 参数。
- 摄像头采集和需要重编码的复杂场景需要后续扩展 libav 编码路径；当前默认实现优先解决 RTSP/文件等输入的 copy/remux 转推。
