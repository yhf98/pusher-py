from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FFMPEG_DIR = ROOT / "third_party" / "FFmpeg"

REQUIRED_FILES = [
    "configure",
    "Makefile",
    "ffbuild/common.mak",
    "ffbuild/library.mak",
    "libavformat/Makefile",
    "libavcodec/Makefile",
    "libavutil/Makefile",
    "libavdevice/Makefile",
    "libavfilter/Makefile",
    "libswscale/Makefile",
    "libswresample/Makefile",
]

REQUIRED_DIRS = [
    "compat",
    "ffbuild",
    "libavformat",
    "libavcodec",
    "libavutil",
    "libavdevice",
    "libavfilter",
    "libswscale",
    "libswresample",
]


def main() -> int:
    missing = []
    for relpath in REQUIRED_FILES:
        if not (FFMPEG_DIR / relpath).is_file():
            missing.append(relpath)
    for relpath in REQUIRED_DIRS:
        if not (FFMPEG_DIR / relpath).is_dir():
            missing.append(relpath + "/")

    if missing:
        print(f"FFmpeg source tree is incomplete: {FFMPEG_DIR}")
        for relpath in missing:
            print(f"Missing required path: {relpath}")
        print("Commit the full third_party/FFmpeg source tree to GitHub before building wheels.")
        return 1

    print(f"FFmpeg source tree looks complete: {FFMPEG_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
