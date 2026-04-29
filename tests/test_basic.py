import sys

import pytest

from pusher import Pusher, build_output_url, detect_protocol, version


def camera_input() -> str:
    return "video=Integrated Camera" if sys.platform == "win32" else "/dev/video0"


def test_version() -> None:
    assert version() == "0.1.9"


@pytest.mark.parametrize(
    ("url", "expected"),
    [
        ("rtmp://127.0.0.1/live/test", "rtmp"),
        ("rtsp://127.0.0.1/live/test", "rtsp"),
        ("srt://127.0.0.1:10080?streamid=live/test", "srt"),
        ("rtp://127.0.0.1:5004", "rtp"),
        ("http://127.0.0.1/rtc/v1/whip/", "whip"),
        ("file:///tmp/video.mp4", "unknown"),
    ],
)
def test_detect_protocol(url: str, expected: str) -> None:
    assert detect_protocol(url) == expected


def test_build_output_url() -> None:
    assert (
        build_output_url("rtmp", "127.0.0.1", app="live", stream="demo", secret="abc")
        == "rtmp://127.0.0.1:1935/live/demo?secret=abc"
    )


def test_pusher_lifecycle() -> None:
    pusher = Pusher(name="unit-test", engine="libav")
    assert not pusher.is_running
    assert pusher.pid == -1
    assert "state=stopped" in pusher.status()


def test_invalid_protocol() -> None:
    pusher = Pusher()
    with pytest.raises(ValueError):
        pusher.start("sample.mp4", "file:///tmp/video.mp4")


def test_rejects_external_ffmpeg_engine() -> None:
    with pytest.raises(ValueError):
        Pusher(engine="ffmpeg")


def test_preview_command_libav() -> None:
    pusher = Pusher(engine="libav", loop=False)
    command = pusher.preview_command("sample.mp4", "rtmp://127.0.0.1/live/test")
    assert command[0] == "libav-remux"
    assert "protocol=rtmp" in command


def test_preview_command_stream_push_for_whip() -> None:
    pusher = Pusher(engine="auto", loop=False)
    command = pusher.preview_command("sample.mp4", "http://127.0.0.1/rtc/v1/whip/?app=live&stream=test")
    assert command[0] == "embedded-whip"
    assert command[1:4] == ["sample.mp4", "->", "http://127.0.0.1/rtc/v1/whip/?app=live&stream=test"]
    assert "loop=0" in command


def test_preview_command_camera_to_rtmp() -> None:
    pusher = Pusher(loop=False, width=640, height=480, fps=25)
    input_url = camera_input()
    command = pusher.preview_command(input_url, "rtmp://127.0.0.1/live/camera")
    assert command[0] == "native-camera-h264"
    assert command[1:4] == [input_url, "->", "rtmp://127.0.0.1/live/camera"]
    assert "protocol=rtmp" in command
    assert "width=640" in command
    assert "height=480" in command
    assert "fps=25" in command


def test_preview_command_camera_to_whip() -> None:
    pusher = Pusher(loop=False, width=640, height=480, fps=25)
    input_url = camera_input()
    command = pusher.preview_command(input_url, "http://127.0.0.1/rtc/v1/whip/?app=live&stream=camera")
    assert command[0] == "native-camera-whip"
    assert command[1:4] == [input_url, "->", "http://127.0.0.1/rtc/v1/whip/?app=live&stream=camera"]
    assert "width=640" in command
    assert "height=480" in command
    assert "fps=25" in command


def test_stream_push_rejects_non_whip_output() -> None:
    pusher = Pusher(engine="stream_push")
    with pytest.raises(ValueError):
        pusher.preview_command("sample.mp4", "rtmp://127.0.0.1/live/test")
