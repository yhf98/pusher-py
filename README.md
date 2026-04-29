# pusher

`pusher` 是一个基于 FFmpeg SDK 二次开发的 Python C/C++ 原生推流/转推 SDK。它直接链接本项目内的 `include/` 与 `lib/`，在当前进程内启动 C++ worker 线程完成工作，不需要为每路流启动一个 `ffmpeg` 命令进程。

完整 Python、C/C++ SDK 方法、参数、默认值、环境变量和 CLI 说明见 [SDK_API.md](SDK_API.md)。

它适合 `-c copy` 这类复制转推场景，例如：

```bash
ffmpeg -rtsp_transport tcp -i rtsp://192.168.0.206:8554/av0_0 \
  -c copy -f flv rtmp://192.168.0.138:1935/live/detect_1500
```

在本项目中对应为：

```python
from pusher import Pusher

pusher = Pusher(loop=False, realtime=False)
pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)
```

## 协议路由

- `rtmp://`、`rtsp://`、`srt://`、`rtp://` 输出：自动使用 FFmpeg SDK 在进程内处理。文件/网络流默认 remux/copy；摄像头输入会采集并编码为 H264 后推流。
- `http(s)://.../rtc/v1/whip/` 输出：自动使用内嵌 WHIP/WebRTC worker 发布 H264 视频。

所有运行时推流路径都在 `_native` 内部执行。100 路复制转推时每路流是一个 C++ worker 线程，不会产生 100 个外部子进程。

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
│   ├── pusher.cpp           # libav 转推和 worker 生命周期
│   ├── pusher_c.cpp         # C ABI，供 C/跨语言 FFI 调用
│   ├── stream_push/whip.cpp # 内嵌 WHIP/WebRTC 推流实现
│   ├── url_utils.c          # 协议识别
│   ├── cli.py               # 命令行工具
│   └── __init__.py
├── examples/basic_usage.py
├── examples/c_demo.c
├── examples/cpp_demo.cpp
├── CMakeLists.txt           # C/C++ SDK 动态库和 demo 构建
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

自动编译采用精简 FFmpeg SDK 配置，主要启用 `avformat`、`avcodec`、`avdevice`、`avfilter`、`avutil`、`swscale`、`swresample`，以及 RTSP/RTMP/RTP/HTTP/TCP/UDP/文件协议、摄像头输入和常见 muxer/demuxer。目标是 native 推流/转推，不包含 ffmpeg/ffprobe 命令行程序。

## CI 发布平台

GitHub Actions 会从仓库内的 `third_party/FFmpeg` 自动构建 FFmpeg SDK，再构建 Python wheel。PyPI wheel 不包含 `third_party/FFmpeg` 源码；Linux wheel 会携带运行所需的 FFmpeg `.so` 动态库。

当前自动发布目标是 Linux `x86_64`、`aarch64`、`armv7l`，以及 Windows `x86`、`x64`、`ARM64`。Linux 使用 `scripts/build_ffmpeg.sh`，Windows 使用 MSVC + MSYS2 执行 `scripts/build_ffmpeg.ps1` 并产出 `.lib`/`.dll` SDK。RK 系列、树莓派、香橙派等开发板通常使用 `aarch64` 64 位系统或 `armv7l` 32 位系统；板载硬编解码能力需要单独的设备镜像/系统库适配，不包含在通用 PyPI wheel 中。

独立 C/C++ 动态库由 `.github/workflows/build-sdk.yml` 构建，只上传 GitHub Actions artifacts，不发布到 PyPI，不影响 Python wheel。当前 SDK artifacts：

| Artifact | 目标 |
| --- | --- |
| `native-sdk-linux-x86` | Linux 32 位 x86 |
| `native-sdk-linux-x86_64` | Linux 64 位 x86 |
| `native-sdk-linux-aarch64` | Linux 64 位 ARM，适合常见 RK/树莓派/香橙派 64 位系统 |
| `native-sdk-linux-armv7l` | Linux 32 位 ARM，适合 armv7/armhf 系统 |
| `native-sdk-windows-x86` | Windows 32 位 x86 |
| `native-sdk-windows-x64` | Windows 64 位 x86 |
| `native-sdk-windows-arm64` | Windows ARM64 |

SDK artifact 内包含 `include/pusher`、`libpusher`/`pusher.dll`、FFmpeg 运行库、C/C++ demo 和文档。通用 SDK CI 默认使用 `PUSHER_WHIP=OFF`，保证 RTMP/RTSP/SRT/RTP 和摄像头 H264 路径可跨架构构建；WHIP/WebRTC 需要目标平台额外提供 `libcurl` 和 `libdatachannel`，可在自有环境用 `PUSHER_WHIP=ON` 单独构建。

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

## C/C++ SDK 构建

C/C++ 第三方库和 Python 扩展共用同一套 native 实现。公共头文件在 `include/pusher/`：

- `pusher/pusher_c.h`：稳定 C ABI，适合 C 项目和跨语言 FFI。
- `pusher/pusher.hpp`：C++17 API，直接使用 `pusher::NativePusher`。
- `pusher/url_utils.h`：输出协议识别。

Linux x86_64、aarch64、armv7l 以及 RK、树莓派、香橙派等常见开发板推荐用 CMake 构建动态库：

```bash
cd /root/workspace/ms-fish-recg-pro/pusher-py
bash scripts/build_ffmpeg.sh
cmake -S . -B build/sdk -DPUSHER_BUILD_EXAMPLES=ON -DPUSHER_WHIP=AUTO
cmake --build build/sdk -j
```

产物：

```text
build/sdk/libpusher.so
build/sdk/pusher_c_demo
build/sdk/pusher_cpp_demo
```

Windows x86/x64/ARM64 使用 Visual Studio Developer Shell，先构建 FFmpeg SDK，再构建 DLL：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build_ffmpeg.ps1 -Arch AMD64
cmake -S . -B build\sdk -G "Visual Studio 17 2022" -A x64 -DPUSHER_BUILD_EXAMPLES=ON -DPUSHER_WHIP=OFF
cmake --build build\sdk --config Release
```

架构对应关系：

| 平台 | CMake/脚本参数 |
| --- | --- |
| Windows x86 | `-Arch x86`，CMake `-A Win32` |
| Windows x64 | `-Arch AMD64`，CMake `-A x64` |
| Windows ARM64 | `-Arch ARM64`，CMake `-A ARM64` |
| Linux x86_64 | 本机 CMake 构建 |
| Linux aarch64 | 本机板端构建，或交叉工具链文件 |
| Linux armv7l | 本机板端构建，或交叉工具链文件 |

交叉编译时传入工具链文件和目标 SDK 路径：

```bash
cmake -S . -B build/aarch64 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/aarch64-toolchain.cmake \
  -DPUSHER_SDK_INCLUDE_DIR=/path/to/target/include \
  -DPUSHER_SDK_LIB_DIR=/path/to/target/lib \
  -DPUSHER_WHIP=OFF
cmake --build build/aarch64 -j
```

`PUSHER_WHIP=ON` 需要目标平台有 `libcurl` 和 `libdatachannel`；嵌入式板如果暂时只需要 RTMP/RTSP/SRT/RTP，先用 `PUSHER_WHIP=OFF` 更稳。

### C/C++ demo

demo 默认只打印预览，不推流：

```bash
build/sdk/pusher_c_demo /dev/video0 rtmp://127.0.0.1:1935/live/camera
build/sdk/pusher_cpp_demo sample.mp4 rtmp://127.0.0.1:1935/live/test
```

真正启动推流需要显式加 `--start`：

```bash
build/sdk/pusher_cpp_demo /dev/video0 rtmp://192.168.0.138:1935/live/camera0 --start
```

打包当前平台 SDK：

```bash
bash scripts/package_sdk.sh linux-x86_64 build/sdk
```

Windows：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_sdk.ps1 -Platform windows-x64 -BuildDir build\sdk -Config Release
```

## 用例总览

下表所有场景都支持 Python SDK；CLI 使用 `PYTHONPATH=src python -m pusher.cli` 展示，安装后可把前缀替换为 `pusher` 或 `pusher-py`。

| 场景 | 输入 | 输出 | 说明 |
| --- | --- | --- | --- |
| RTSP 转 RTMP | `rtsp://...` | `rtmp://...` | 摄像头或网络流复制转推。 |
| RTSP 转 RTMP 带鉴权 | `rtsp://...` | `rtmp://...?secret=...` | 用 `build_output_url` 或 `build-url` 生成地址。 |
| 本地 MP4 循环推 RTMP | `sample.mp4` | `rtmp://...` | `loop=True`，按时间戳实时推送。 |
| 本地 MP4 单次推 RTMP | `sample.mp4` | `rtmp://...` | `loop=False`，文件结束后退出。 |
| RTSP 转 RTSP | `rtsp://...` | `rtsp://...` | 输出侧默认 TCP。 |
| RTSP 转 SRT | `rtsp://...` | `srt://...` | 要求当前 FFmpeg SDK 启用 SRT。 |
| RTSP 转 RTP | `rtsp://...` | `rtp://...` | 单路 RTP 输出。 |
| MP4 推 WHIP/WebRTC | H264 MP4 | WHIP HTTP URL | 走内嵌 WHIP/WebRTC worker，不启动外部程序。 |
| 摄像头推 RTMP/RTSP/SRT/RTP | `/dev/video0` 或 `video=设备名` | `rtmp://...` 等 | native 采集并编码为 H264 后推流。 |
| 摄像头推 WHIP/WebRTC | `/dev/video0` 或 `video=设备名` | WHIP HTTP URL | native 采集、H264 编码、内嵌 WHIP 推流。 |
| 预览任务 | 任意支持输入 | 任意支持输出 | 打印 native 任务描述，不启动推流。 |
| 协议识别和 URL 构造 | URL 参数 | URL 字符串 | 业务侧生成和检查推流地址。 |
| 多路转推 | 多个输入 | 多个输出 | Python SDK 推荐，一个实例一个 native worker 线程。 |

注意：`start()` 会启动后台 C++ worker 线程并立即返回，业务进程需要保持运行。脚本如果启动后直接退出，`Pusher` 对象析构会停止转推。

## Python 用例

### 1. RTSP 转 RTMP

```python
import time

from pusher import Pusher

pusher = Pusher(
    log_path="pusher.log",
    loop=False,
    realtime=False,
    timeout_ms=5000,
)

pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)

try:
    while pusher.is_running:
        print(pusher.status())
        time.sleep(5)
finally:
    pusher.stop(3000)
```

### 2. RTSP 转 RTMP 带鉴权地址

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

pusher = Pusher(loop=False, realtime=False)
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

### 3. 本地 MP4 循环推 RTMP

```python
from pusher import Pusher

pusher = Pusher(loop=True, realtime=True)
pusher.start("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
print(pusher.status())
```

### 4. 本地 MP4 单次推 RTMP

```python
from pusher import Pusher

pusher = Pusher(loop=False, realtime=True)
pusher.start("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
exit_code = pusher.wait(timeout_ms=-1)
print("exit:", exit_code)
```

### 5. RTSP 转 RTSP

```python
from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="rtsp",
    host="192.168.0.138",
    app="live",
    stream="detect_1500",
    port=8554,
)

pusher = Pusher(loop=False, realtime=False)
pusher.start("rtsp://192.168.0.206:8554/av0_0", output_url)
print(pusher.status())
```

### 6. RTSP 转 SRT

```python
from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="srt",
    host="192.168.0.138",
    app="live",
    stream="detect_1500",
    port=10080,
)

pusher = Pusher(loop=False, realtime=False)
pusher.start("rtsp://192.168.0.206:8554/av0_0", output_url)
print(pusher.status())
```

SRT 需要当前 FFmpeg SDK 编译时启用 `srt` 协议；默认脚本未启用外部 `libsrt` 依赖。

### 7. RTSP 转 RTP

```python
from pusher import Pusher

pusher = Pusher(loop=False, realtime=False)
pusher.start(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtp://192.168.0.138:5004",
)
print(pusher.status())
```

### 8. MP4 推 WHIP/WebRTC

WHIP 不走 FFmpeg SDK muxer，输出 URL 为 WHIP 地址时会自动选择内嵌 WHIP/WebRTC worker。它不会执行 `whip-push-demo` 或任何 `stream_push` 可执行文件。文件或网络输入推 WHIP 时要求输入视频已经是 H264；摄像头输入会在 native 路径中编码为 H264。

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

pusher = Pusher(loop=True, bitrate=2_000_000)
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

### 9. 预览任务但不启动

```python
from pusher import Pusher

pusher = Pusher(loop=False, realtime=False)
command = pusher.preview_command(
    "rtsp://192.168.0.206:8554/av0_0",
    "rtmp://192.168.0.138:1935/live/detect_1500",
)
print(" ".join(command))
```

### 10. 停止、等待和上下文管理

```python
from pusher import Pusher

with Pusher(loop=False, realtime=False) as pusher:
    pusher.start("sample.mp4", "rtmp://127.0.0.1:1935/live/test")
    exit_code = pusher.wait(timeout_ms=10_000)
    if exit_code is None:
        pusher.stop(timeout_ms=3000)
```

### 11. 多路转推

```python
from pusher import Pusher

tasks = []
for i in range(100):
    p = Pusher(name=f"stream-{i}", loop=False, realtime=False)
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

### 12. 协议识别和 URL 构造

```python
from pusher import build_output_url, detect_protocol, version

print(version())
print(detect_protocol("rtmp://192.168.0.138:1935/live/test"))

for protocol in ["rtmp", "rtsp", "srt", "rtp", "whip"]:
    print(protocol, build_output_url(protocol, "192.168.0.138", stream="demo"))
```

### 13. 查找摄像头输入标识

摄像头标识不是固定值，需要从操作系统查询。Linux 使用 V4L2 设备路径；Windows 使用 DirectShow 设备名。下面这些命令只用于列设备，不参与实际推流，推流仍然由 native worker 完成。

Linux 常用方式：

```bash
ls -l /dev/video*
ls -l /dev/v4l/by-id/
ls -l /dev/v4l/by-path/
```

`/dev/video0`、`/dev/video1` 这类编号可能会随插拔顺序变化；生产环境更推荐使用稳定路径，例如：

```text
/dev/v4l/by-id/usb-046d_HD_Pro_Webcam_C920-video-index0
/dev/v4l/by-path/pci-0000:00:14.0-usb-0:5:1.0-video-index0
```

如果系统安装了 `v4l-utils`，可以查看摄像头名称、设备路径、支持的分辨率和编码格式：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

典型输出类似：

```text
HD Pro Webcam C920 (usb-0000:00:14.0-5):
    /dev/video0
    /dev/video1
```

这时通常选择带视频采集能力的 `/dev/video0`，或选择 `/dev/v4l/by-id/...video-index0` 稳定路径。`--list-formats-ext` 中如果能看到 `H264`，SDK 会优先尝试直接读取 H264；如果只有 `MJPG`、`YUYV` 等格式，SDK 会走 native 解码/编码为 H264。

Windows 使用 DirectShow 设备名，输入格式是：

```text
video=摄像头名称
```

可以在 PowerShell 里列出摄像头友好名称：

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.PNPClass -eq "Camera" -or $_.Name -match "Camera|Webcam|Video" } |
  Select-Object Name
```

也可以在“设备管理器 -> 照相机/图像设备”里查看名称。假设名称是 `Integrated Camera`，传给 SDK/CLI 的输入就是：

```text
video=Integrated Camera
```

如果同名设备不止一个，建议在系统里修改设备名称，或者后续扩展 SDK 增加设备枚举 API 后按枚举结果选择。

### 14. 摄像头推 RTMP/RTSP/SRT/RTP

摄像头输入使用 native FFmpeg SDK 采集；如果摄像头直接输出 H264 会直接封装，否则会解码后编码为 H264。不会启动外部 `ffmpeg`。
Linux 使用 `/dev/video0` 这类 V4L2 路径；Windows 使用 dshow 设备名，例如 `video=Integrated Camera`。

```python
from pusher import Pusher

pusher = Pusher(
    width=1280,
    height=720,
    fps=30,
    bitrate=2_000_000,
    loop=False,
    realtime=False,
)
pusher.start("/dev/video0", "rtmp://192.168.0.138:1935/live/camera0")
print(pusher.status())
```

同一个摄像头输入也可以输出到 RTSP/SRT/RTP：

```python
from pusher import Pusher

pusher = Pusher(width=1280, height=720, fps=30, bitrate=2_000_000)
pusher.start("/dev/video0", "rtsp://192.168.0.138:8554/live/camera0")
```

Windows 摄像头输入示例：

```python
from pusher import Pusher

pusher = Pusher(width=1280, height=720, fps=30, bitrate=2_000_000)
pusher.start("video=Integrated Camera", "rtmp://192.168.0.138:1935/live/camera0")
```

### 15. 摄像头推 WHIP/WebRTC

```python
import time

from pusher import Pusher, build_output_url

output_url = build_output_url(
    protocol="whip",
    host="192.168.0.138",
    app="live",
    stream="camera0",
    port=1985,
)

pusher = Pusher(width=1280, height=720, fps=30, bitrate=2_000_000)
pusher.start("/dev/video0", output_url)

try:
    while pusher.is_running:
        print(pusher.status())
        time.sleep(5)
finally:
    pusher.stop(3000)
```

摄像头路径要求 FFmpeg SDK 启用 `avdevice`、`swscale` 和至少一个 H264 编码器，例如 `libx264`、`h264_v4l2m2m`、`h264_mf` 或平台硬编码器。

## CLI 用例

源码目录示例统一使用 `PYTHONPATH=src python -m pusher.cli`。开发安装后可以改成 `pusher`：

```bash
pusher push input output_url
pusher preview input output_url --json
pusher build-url rtmp 192.168.0.138 --stream demo
```

### 1. RTSP 转 RTMP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1500 \
  --no-loop \
  --no-realtime \
  --log-path pusher.log \
  --status-interval 5
```

### 2. RTSP 转 RTMP 带鉴权地址

```bash
PYTHONPATH=src python -m pusher.cli build-url rtmp 192.168.0.138 \
  --app live \
  --stream test \
  --secret 557ea19cf905454bad9dc988d0c6a5g1

PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  'rtmp://192.168.0.138:1935/live/test?secret=557ea19cf905454bad9dc988d0c6a5g1' \
  --no-loop \
  --no-realtime \
  --status-interval 5
```

### 3. 本地 MP4 循环推 RTMP

```bash
PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  rtmp://127.0.0.1:1935/live/test \
  --status-interval 5
```

### 4. 本地 MP4 单次推 RTMP

```bash
PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  rtmp://127.0.0.1:1935/live/test \
  --no-loop \
  --status-interval 5
```

### 5. RTSP 转 RTSP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtsp://192.168.0.138:8554/live/detect_1500 \
  --no-loop \
  --no-realtime
```

### 6. RTSP 转 SRT

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  'srt://192.168.0.138:10080?streamid=live/detect_1500' \
  --no-loop \
  --no-realtime
```

SRT 需要当前 FFmpeg SDK 编译时启用 `srt` 协议；默认脚本未启用外部 `libsrt` 依赖。

### 7. RTSP 转 RTP

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.206:8554/av0_0 \
  rtp://192.168.0.138:5004 \
  --no-loop \
  --no-realtime
```

### 8. MP4 推 WHIP/WebRTC

```bash
PYTHONPATH=src python -m pusher.cli build-url whip 192.168.0.138 \
  --app live \
  --stream test \
  --secret replace-me \
  --port 1985

PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test&secret=replace-me' \
  --status-interval 5
```

### 9. 预览任务但不启动

```bash
PYTHONPATH=src python -m pusher.cli preview \
  rtsp://192.168.0.206:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1500 \
  --no-loop \
  --no-realtime

PYTHONPATH=src python -m pusher.cli preview \
  sample.mp4 \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test' \
  --json
```

### 10. 停止和等待

```bash
PYTHONPATH=src python -m pusher.cli push \
  sample.mp4 \
  rtmp://127.0.0.1:1935/live/test \
  --status-interval 5
```

CLI 会等待推流结束。按 `Ctrl+C` 或发送 `SIGTERM` 时会调用 native `stop()`，不会启动或清理外部推流进程。

### 11. 多路转推

CLI 每条命令是一个 Python 进程，适合调试和少量任务；大量任务推荐使用上面的 Python 多实例方式统一管理。

```bash
PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.101:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_1 \
  --no-loop \
  --no-realtime

PYTHONPATH=src python -m pusher.cli push \
  rtsp://192.168.0.102:8554/av0_0 \
  rtmp://192.168.0.138:1935/live/detect_2 \
  --no-loop \
  --no-realtime
```

### 12. 协议识别和 URL 构造

```bash
PYTHONPATH=src python -m pusher.cli detect \
  rtmp://192.168.0.138:1935/live/test

PYTHONPATH=src python -m pusher.cli detect \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=test'

PYTHONPATH=src python -m pusher.cli build-url rtmp 192.168.0.138 \
  --app live \
  --stream demo

PYTHONPATH=src python -m pusher.cli build-url whip 192.168.0.138 \
  --app live \
  --stream demo \
  --port 1985 \
  --tls
```

### 13. 查找摄像头输入标识

CLI 使用的摄像头输入标识和 Python SDK 相同。Linux 先用下面命令确认设备：

```bash
ls -l /dev/video*
ls -l /dev/v4l/by-id/
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Windows 先在 PowerShell 查询设备名：

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.PNPClass -eq "Camera" -or $_.Name -match "Camera|Webcam|Video" } |
  Select-Object Name
```

Linux CLI 输入示例是 `/dev/video0` 或 `/dev/v4l/by-id/...`；Windows CLI 输入示例是 `"video=Integrated Camera"`。

### 14. 摄像头推 RTMP/RTSP/SRT/RTP

```bash
PYTHONPATH=src python -m pusher.cli push \
  /dev/video0 \
  rtmp://192.168.0.138:1935/live/camera0 \
  --width 1280 \
  --height 720 \
  --fps 30 \
  --bitrate 2000000 \
  --no-loop \
  --no-realtime \
  --status-interval 5

PYTHONPATH=src python -m pusher.cli push \
  /dev/video0 \
  rtsp://192.168.0.138:8554/live/camera0 \
  --width 1280 \
  --height 720 \
  --fps 30 \
  --bitrate 2000000 \
  --status-interval 5
```

Windows CLI 示例：

```powershell
python -m pusher.cli push `
  "video=Integrated Camera" `
  rtmp://192.168.0.138:1935/live/camera0 `
  --width 1280 `
  --height 720 `
  --fps 30 `
  --bitrate 2000000 `
  --status-interval 5
```

### 15. 摄像头推 WHIP/WebRTC

```bash
PYTHONPATH=src python -m pusher.cli build-url whip 192.168.0.138 \
  --app live \
  --stream camera0 \
  --port 1985

PYTHONPATH=src python -m pusher.cli push \
  /dev/video0 \
  'http://192.168.0.138:1985/rtc/v1/whip/?app=live&stream=camera0' \
  --width 1280 \
  --height 720 \
  --fps 30 \
  --bitrate 2000000 \
  --status-interval 5
```

摄像头推流仍然是 native worker，不执行外部 `ffmpeg`、`stream_push` 或 `whip-push-demo`。

## 支持协议

输出协议映射：

| 输出 URL | 自动处理方式 | 输出封装/程序 |
| --- | --- | --- |
| `rtmp://` / `rtmps://` | FFmpeg SDK remux | `flv` |
| `rtsp://` / `rtsps://` | FFmpeg SDK remux | `rtsp` |
| `srt://` | FFmpeg SDK remux | `mpegts`，要求当前 FFmpeg SDK 编译时启用 `srt` 协议 |
| `rtp://` | FFmpeg SDK remux | `rtp` |
| `http://.../rtc/v1/whip/` / `https://.../rtc/v1/whip/` | 内嵌 WHIP worker | WHIP/WebRTC |

WHIP/WebRTC 不属于 FFmpeg 常规 muxer 输出，本项目使用内嵌 WHIP/WebRTC worker 处理。

## 参数说明

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `log_path` | 空 | libav 运行日志 |
| `loop` | `True` | 本地文件读到 EOF 后是否循环 |
| `realtime` | `True` | 本地文件是否按媒体时间戳限速 |
| `timeout_ms` | `5000` | 网络打开/停止等待超时 |
| `analyzeduration_us` | `10000000` | RTSP/网络流探测时长，单位微秒 |
| `probesize` | `50000000` | RTSP/网络流探测数据量，摄像头首包缺少尺寸时可增大 |

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

- 文件和网络流默认 remux/copy 转推；摄像头输入会 native 采集并编码为 H264。
- 运行时不启动外部 `ffmpeg` 或 `stream_push` 程序；所有推流路径都在 native worker 内执行。
- 如果输入编码或目标协议不被当前 FFmpeg SDK 支持，任务会退出并在 `status()` 或日志中给出错误。
- 自动编译脚本默认不启用外部 `libsrt` 依赖；如需 SRT，请在系统安装 libsrt 开发包后调整 `scripts/build_ffmpeg.sh` 的 configure 参数。
- 摄像头输入会走 native 采集和 H264 编码；对应 FFmpeg SDK 必须包含 `avdevice`、`swscale` 和可用 H264 编码器。
