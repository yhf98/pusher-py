from __future__ import annotations

import sys
import subprocess
import os
import shlex
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


ROOT = Path(__file__).parent
LOCAL_INCLUDE = ROOT / "include"
LOCAL_LIB = ROOT / "lib"
IS_WINDOWS = sys.platform == "win32"
FFMPEG_COMPONENTS = ("avformat", "avcodec", "avutil", "avdevice", "avfilter", "swscale", "swresample")


def windows_import_library(component: str):
    for name in (component, f"lib{component}"):
        if (LOCAL_LIB / f"{name}.lib").exists():
            return name
    return None


def has_windows_ffmpeg() -> bool:
    return all(
        windows_import_library(component)
        and any(LOCAL_LIB.glob(f"*{component}*.dll"))
        for component in FFMPEG_COMPONENTS
    )


def has_local_ffmpeg() -> bool:
    if IS_WINDOWS:
        return has_windows_ffmpeg()
    return (LOCAL_LIB / "libavformat.so").exists()


def require_local_ffmpeg() -> None:
    if has_local_ffmpeg():
        return

    if IS_WINDOWS:
        found = ", ".join(sorted(path.name for path in LOCAL_LIB.glob("*"))) if LOCAL_LIB.exists() else "<missing lib/>"
        missing = []
        for component in FFMPEG_COMPONENTS:
            if not windows_import_library(component):
                missing.append(f"{component}.lib")
            if not any(LOCAL_LIB.glob(f"*{component}*.dll")):
                missing.append(f"{component}.dll")
        raise RuntimeError(
            "local FFmpeg Windows SDK is incomplete. "
            f"Missing: {', '.join(missing)}. Found in lib/: {found}"
        )

    raise RuntimeError("local FFmpeg SDK is incomplete. Expected lib/libavformat.so")


def ffmpeg_library_names() -> list[str]:
    if not IS_WINDOWS:
        return list(FFMPEG_COMPONENTS)

    libraries = []
    for component in FFMPEG_COMPONENTS:
        library = windows_import_library(component)
        if library is None:
            raise RuntimeError(f"missing FFmpeg import library for {component}")
        libraries.append(library)
    return libraries


def run_pkg_config(args: list[str]) -> list[str]:
    try:
        result = subprocess.run(
            ["pkg-config", *args],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return []
    return shlex.split(result.stdout)


def parse_pkg_config_flags(flags: list[str]) -> tuple[list[str], list[str], list[str], list[str]]:
    include_dirs: list[str] = []
    library_dirs: list[str] = []
    libraries: list[str] = []
    extra_link_args: list[str] = []

    for flag in flags:
        if flag.startswith("-I") and len(flag) > 2:
            include_dirs.append(flag[2:])
        elif flag.startswith("-L") and len(flag) > 2:
            library_dirs.append(flag[2:])
        elif flag.startswith("-l") and len(flag) > 2:
            libraries.append(flag[2:])
        else:
            extra_link_args.append(flag)

    return include_dirs, library_dirs, libraries, extra_link_args


def detect_whip_support() -> tuple[list[str], list[str], list[str], list[str], list[tuple[str, str]]]:
    if IS_WINDOWS:
        return [], [], [], [], []

    rtc_header = Path("/usr/local/include/rtc/rtc.hpp")
    datachannel_lib = Path("/usr/local/lib/libdatachannel.so")
    curl_flags = run_pkg_config(["--cflags", "--libs", "libcurl"])
    if not rtc_header.exists() or not datachannel_lib.exists() or not curl_flags:
        return [], [], [], [], []

    include_dirs, library_dirs, libraries, extra_link_args = parse_pkg_config_flags(curl_flags)
    include_dirs.append("/usr/local/include")
    library_dirs.append("/usr/local/lib")
    libraries.append("datachannel")
    extra_link_args.append("-Wl,-rpath,/usr/local/lib")

    return (
        list(dict.fromkeys(include_dirs)),
        list(dict.fromkeys(library_dirs)),
        list(dict.fromkeys(libraries)),
        list(dict.fromkeys(extra_link_args)),
        [("PUSHER_ENABLE_WHIP", "1")],
    )


def ensure_soname_links() -> None:
    if not LOCAL_LIB.exists():
        return

    for shared_lib in LOCAL_LIB.glob("*.so"):
        try:
            result = subprocess.run(
                ["readelf", "-d", str(shared_lib)],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
        except (OSError, subprocess.CalledProcessError):
            continue

        soname = ""
        for line in result.stdout.splitlines():
            marker = "Library soname: ["
            if marker in line:
                soname = line.split(marker, 1)[1].split("]", 1)[0]
                break

        if not soname or soname == shared_lib.name:
            continue

        link_path = LOCAL_LIB / soname
        if not link_path.exists():
            os.symlink(shared_lib.name, link_path)


def ensure_local_ffmpeg() -> None:
    if has_local_ffmpeg():
        if not IS_WINDOWS:
            ensure_soname_links()
        return

    source_dir = ROOT / "third_party" / "FFmpeg"
    script = ROOT / "scripts" / ("build_ffmpeg.ps1" if IS_WINDOWS else "build_ffmpeg.sh")
    if not script.exists() or not source_dir.exists():
        raise RuntimeError(
            "local FFmpeg SDK is missing. Expected platform FFmpeg libraries in lib/, "
            f"or bundled source at third_party/FFmpeg plus {script.relative_to(ROOT)}"
        )

    if IS_WINDOWS:
        subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
            ],
            cwd=str(ROOT),
            check=True,
        )
        require_local_ffmpeg()
    else:
        subprocess.run([str(script)], cwd=str(ROOT), check=True)
        ensure_soname_links()
        require_local_ffmpeg()


ensure_local_ffmpeg()

ffmpeg_cflags = [f"-I{LOCAL_INCLUDE}"]
ffmpeg_libraries = ffmpeg_library_names()
whip_include_dirs, whip_library_dirs, whip_libraries, whip_link_args, whip_macros = detect_whip_support()

ffmpeg_runtime_args: list[str] = []
if sys.platform.startswith("linux") and LOCAL_LIB.exists():
    ffmpeg_runtime_args.extend([
        f"-Wl,-rpath,{LOCAL_LIB}",
        "-Wl,-rpath,$ORIGIN/../../lib",
    ])


class BuildExt(build_ext):
    """Apply platform-specific compiler flags for the native extension."""

    def build_extensions(self) -> None:
        compiler_type = self.compiler.compiler_type
        for ext in self.extensions:
            link_args = list(ext.extra_link_args or [])
            if compiler_type == "msvc":
                ext.extra_compile_args = ["/O2", "/EHsc"]
            else:
                ext.extra_compile_args = ["-O3", "-Wall", "-Wextra", *ffmpeg_cflags]
                if sys.platform == "darwin":
                    ext.extra_compile_args.extend(["-stdlib=libc++"])
                    link_args.append("-stdlib=libc++")
            ext.extra_link_args = link_args

        original_compile = self.compiler._compile

        def compile_with_source_flags(obj, src, ext, cc_args, extra_postargs, pp_opts):
            postargs = list(extra_postargs or [])
            if Path(src).suffix.lower() in {".cc", ".cpp", ".cxx"}:
                postargs.append("/std:c++17" if compiler_type == "msvc" else "-std=c++17")
            return original_compile(obj, src, ext, cc_args, postargs, pp_opts)

        self.compiler._compile = compile_with_source_flags
        super().build_extensions()


native_extension = Extension(
    "pusher._native",
    sources=[
        "src/pusher/_native.cpp",
        "src/pusher/camera_source.cpp",
        "src/pusher/pusher.cpp",
        "src/pusher/stream_push/whip.cpp",
        "src/pusher/url_utils.c",
    ],
    include_dirs=[str(ROOT / "include"), *whip_include_dirs],
    library_dirs=[str(LOCAL_LIB), *whip_library_dirs],
    libraries=[*ffmpeg_libraries, *whip_libraries],
    extra_link_args=[*ffmpeg_runtime_args, *whip_link_args],
    language="c++",
    define_macros=[("PY_SSIZE_T_CLEAN", None), *whip_macros],
)


setup(
    ext_modules=[native_extension],
    cmdclass={"build_ext": BuildExt},
)
