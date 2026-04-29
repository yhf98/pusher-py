from __future__ import annotations

import argparse
import json
import signal
import sys
import time
from typing import Sequence

from . import Pusher, build_output_url, detect_protocol


def _add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--log-path", default="")
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--bitrate", type=int, default=2_000_000)
    parser.add_argument("--analyzeduration-us", type=int, default=10_000_000)
    parser.add_argument("--probesize", type=int, default=50_000_000)
    parser.add_argument("--no-loop", action="store_true")
    parser.add_argument("--no-realtime", action="store_true")


def _make_pusher(args: argparse.Namespace) -> Pusher:
    return Pusher(
        name=getattr(args, "name", "cli"),
        timeout_ms=args.timeout_ms,
        log_path=args.log_path,
        loop=not args.no_loop,
        realtime=not args.no_realtime,
        width=args.width,
        height=args.height,
        fps=args.fps,
        bitrate=args.bitrate,
        analyzeduration_us=args.analyzeduration_us,
        probesize=args.probesize,
    )


def cmd_push(args: argparse.Namespace) -> int:
    pusher = _make_pusher(args)
    print("protocol:", detect_protocol(args.output_url))
    print("command:", " ".join(pusher.preview_command(args.input, args.output_url)))

    stopping = False

    def stop_handler(signum: int, _frame: object) -> None:
        nonlocal stopping
        if stopping:
            return
        stopping = True
        print(f"\nreceived signal {signum}, stopping...", file=sys.stderr)
        pusher.stop(args.timeout_ms)

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)

    pusher.start(args.input, args.output_url)
    print("started:", pusher.status())

    while pusher.is_running:
        if args.status_interval > 0:
            print(pusher.status())
            time.sleep(args.status_interval)
        else:
            time.sleep(0.2)

    exit_code = pusher.exit_code
    return 0 if exit_code is None else int(exit_code)


def cmd_build_url(args: argparse.Namespace) -> int:
    url = build_output_url(
        protocol=args.protocol,
        host=args.host,
        app=args.app,
        stream=args.stream,
        secret=args.secret,
        port=args.port,
        use_tls=args.tls,
    )
    print(url)
    return 0


def cmd_detect(args: argparse.Namespace) -> int:
    print(detect_protocol(args.url))
    return 0


def cmd_preview(args: argparse.Namespace) -> int:
    pusher = _make_pusher(args)
    command = pusher.preview_command(args.input, args.output_url)
    if args.json:
        print(json.dumps(command, ensure_ascii=False))
    else:
        print(" ".join(command))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pusher", description="Python C/C++ native stream pusher tool.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    push = subparsers.add_parser("push", help="Start a native push worker.")
    push.add_argument("input")
    push.add_argument("output_url")
    push.add_argument("--name", default="cli")
    push.add_argument("--status-interval", type=float, default=0.0)
    _add_common_options(push)
    push.set_defaults(func=cmd_push)

    preview = subparsers.add_parser("preview", help="Print the native worker description without starting it.")
    preview.add_argument("input")
    preview.add_argument("output_url")
    preview.add_argument("--name", default="preview")
    preview.add_argument("--json", action="store_true")
    _add_common_options(preview)
    preview.set_defaults(func=cmd_preview)

    build_url = subparsers.add_parser("build-url", help="Build an output URL.")
    build_url.add_argument("protocol", choices=["rtmp", "rtsp", "srt", "rtp", "whip"])
    build_url.add_argument("host")
    build_url.add_argument("--app", default="live")
    build_url.add_argument("--stream", default="test")
    build_url.add_argument("--secret", default="")
    build_url.add_argument("--port", type=int, default=0)
    build_url.add_argument("--tls", action="store_true")
    build_url.set_defaults(func=cmd_build_url)

    detect = subparsers.add_parser("detect", help="Detect protocol from output URL.")
    detect.add_argument("url")
    detect.set_defaults(func=cmd_detect)

    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
