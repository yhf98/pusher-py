#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: scripts/package_sdk.sh <platform> [build-dir]" >&2
  exit 2
fi

PLATFORM="$1"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${2:-${ROOT_DIR}/build/sdk}"
VERSION="$(sed -n 's/^#define PUSHER_VERSION "\(.*\)"/\1/p' "${ROOT_DIR}/include/pusher/version.h" | head -1)"

if [[ -z "${VERSION}" ]]; then
  echo "Unable to read PUSHER_VERSION from include/pusher/version.h" >&2
  exit 1
fi

PACKAGE_NAME="pusher-sdk-${VERSION}-${PLATFORM}"
DIST_DIR="${ROOT_DIR}/dist-sdk"
STAGING_DIR="${DIST_DIR}/${PACKAGE_NAME}"
ARCHIVE="${DIST_DIR}/${PACKAGE_NAME}.tar.gz"

rm -rf "${STAGING_DIR}" "${ARCHIVE}"
mkdir -p \
  "${STAGING_DIR}/bin" \
  "${STAGING_DIR}/examples" \
  "${STAGING_DIR}/include" \
  "${STAGING_DIR}/lib"

cp -a "${ROOT_DIR}/include/pusher" "${STAGING_DIR}/include/"
cp -a "${ROOT_DIR}/examples/c_demo.c" "${ROOT_DIR}/examples/cpp_demo.cpp" "${STAGING_DIR}/examples/"
cp -a "${ROOT_DIR}/README.md" "${ROOT_DIR}/SDK_API.md" "${ROOT_DIR}/CMakeLists.txt" "${STAGING_DIR}/"

cp -a "${BUILD_DIR}"/libpusher.so* "${STAGING_DIR}/lib/"
if [[ -x "${BUILD_DIR}/pusher_c_demo" ]]; then
  cp -a "${BUILD_DIR}/pusher_c_demo" "${STAGING_DIR}/bin/"
fi
if [[ -x "${BUILD_DIR}/pusher_cpp_demo" ]]; then
  cp -a "${BUILD_DIR}/pusher_cpp_demo" "${STAGING_DIR}/bin/"
fi

find "${ROOT_DIR}/lib" -maxdepth 1 \( -type f -o -type l \) \
  \( -name "*.so" -o -name "*.so.*" \) \
  -exec cp -a {} "${STAGING_DIR}/lib/" \;

cat > "${STAGING_DIR}/README_SDK.txt" <<EOF
pusher native SDK ${VERSION} (${PLATFORM})

Contents:
- include/pusher: public C ABI and C++ headers
- lib: libpusher and bundled FFmpeg runtime libraries
- bin: C and C++ demo programs
- examples: demo source code

Runtime:
  export LD_LIBRARY_PATH="\$(pwd)/lib:\${LD_LIBRARY_PATH:-}"

Dry-run demos:
  ./bin/pusher_c_demo sample.mp4 rtmp://127.0.0.1:1935/live/test
  ./bin/pusher_cpp_demo /dev/video0 rtmp://127.0.0.1:1935/live/camera

Start a real push only when the target server is ready:
  ./bin/pusher_cpp_demo /dev/video0 rtmp://SERVER/live/camera0 --start
EOF

tar -C "${DIST_DIR}" -czf "${ARCHIVE}" "${PACKAGE_NAME}"
echo "Created ${ARCHIVE}"
