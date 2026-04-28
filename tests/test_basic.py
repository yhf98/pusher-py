import os

import pytest

from pusher import Pusher, build_output_url, detect_protocol, version


def test_version() -> None:
    assert version() == "0.1.8"


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
    if os.name == "nt":
        pytest.skip("Windows CI does not provide a native ffmpeg stub executable.")

    pusher = Pusher(name="unit-test", engine="ffmpeg", ffmpeg_path="/bin/true")
    assert not pusher.is_running

    pusher.start("rtsp://127.0.0.1/live/source", "rtmp://127.0.0.1/live/test")
    assert pusher.protocol == "rtmp"
    assert pusher.engine == "ffmpeg"

    exit_code = pusher.wait(timeout_ms=3000)
    assert exit_code == 0
    assert not pusher.is_running


def test_invalid_protocol() -> None:
    pusher = Pusher()
    with pytest.raises(ValueError):
        pusher.start("sample.mp4", "file:///tmp/video.mp4")


def test_preview_command_ffmpeg() -> None:
    pusher = Pusher(engine="ffmpeg", ffmpeg_path="ffmpeg", loop=False)
    command = pusher.preview_command("sample.mp4", "rtmp://127.0.0.1/live/test")
    assert command[:4] == ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel"]
    assert "-f" in command
    assert "flv" in command


def test_preview_command_libav() -> None:
    pusher = Pusher(engine="libav", loop=False)
    command = pusher.preview_command("sample.mp4", "rtmp://127.0.0.1/live/test")
    assert command[0] == "libav-remux"
    assert "protocol=rtmp" in command


def test_preview_command_stream_push_for_whip() -> None:
    pusher = Pusher(engine="auto", stream_push_path="/opt/stream_push", loop=False)
    command = pusher.preview_command("sample.mp4", "http://127.0.0.1/rtc/v1/whip/?app=live&stream=test")
    assert command[0] == "/opt/stream_push"
    assert command[-2:] == ["sample.mp4", "http://127.0.0.1/rtc/v1/whip/?app=live&stream=test"]
