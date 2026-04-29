# Repository Guidelines

## Project Structure & Module Organization

This is a Python package with a native C/C++ streaming extension. Core code lives in `src/pusher/`: `__init__.py` exposes the API, `cli.py` defines `pusher`, and `_native.cpp`, `pusher.cpp`, and `url_utils.c` implement the extension. Public headers are in `include/pusher/`; FFmpeg SDK headers are under `include/libav*`, with local shared libraries in `lib/`. `third_party/FFmpeg/` feeds the SDK build. Tests live in `tests/`, examples in `examples/`, and docs in `README.md` and `SDK_API.md`.

## Build, Test, and Development Commands

- `python -m pip install -e ".[dev]"`: install the package in editable mode with build and pytest dependencies.
- `python setup.py build_ext --inplace --force`: rebuild the native extension. If `lib/libavformat.so` is missing, this invokes `scripts/build_ffmpeg.sh`.
- `powershell -File scripts/build_ffmpeg.ps1 -Arch AMD64`: build the Windows FFmpeg SDK from a Visual Studio Developer shell; use `x86` or `ARM64` for other Windows wheels.
- `python -m pytest -q`: run the pytest suite configured in `pyproject.toml`.
- `PYTHONPATH=src python -m pusher.cli preview sample.mp4 rtmp://127.0.0.1/live/test`: inspect the generated native worker description without starting a real stream.

## Coding Style & Naming Conventions

Use 4-space indentation for Python, type hints for public or non-trivial functions, `snake_case` for functions and variables, and `PascalCase` for classes. Keep CLI flags kebab-case while mapping them to snake_case Python attributes. Native code is C++17; prefer small RAII helpers, keep C helpers in `snake_case`, and avoid `-Wall -Wextra` warnings. Do not edit vendored FFmpeg files unless updating the SDK snapshot.

## Testing Guidelines

Tests use pytest, with files named `test_*.py` and test functions named `test_*`. Prefer fast unit tests for URL building, protocol detection, CLI preview output, and lifecycle behavior that avoids real network streams. Existing tests use `preview_command()` to keep native behavior deterministic. After native or packaging changes, rebuild with `build_ext` before running pytest.

## Commit & Pull Request Guidelines

Git history uses Conventional Commit-style prefixes such as `feat:`, `fix:`, and `refactor:`, sometimes with scopes like `feat(native):`. Follow that pattern with a concise subject, for example `fix(cli): validate preview output URL`. Pull requests should include a short behavior summary, test results, any FFmpeg/native build impact, linked issues when applicable, and documentation updates for public API or CLI changes.

## Security & Configuration Tips

Keep generated artifacts out of commits when possible: `build/`, `dist/`, `*.egg-info`, caches, and compiled extension outputs are ignored. Avoid committing machine-specific paths, credentials, or stream secrets in examples and logs.
