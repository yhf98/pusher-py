#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${ROOT_DIR}/third_party/FFmpeg"
BUILD_DIR="${ROOT_DIR}/build/ffmpeg"
PREFIX="${ROOT_DIR}/.ffmpeg-prefix"

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "FFmpeg source not found: ${SRC_DIR}" >&2
  exit 1
fi

required_sources=(
  "configure"
  "Makefile"
  "ffbuild/common.mak"
  "ffbuild/library.mak"
  "libavformat/Makefile"
  "libavcodec/Makefile"
  "libavutil/Makefile"
  "libavdevice/Makefile"
  "libswscale/Makefile"
  "libswresample/Makefile"
)
missing_sources=()
for source_file in "${required_sources[@]}"; do
  if [[ ! -f "${SRC_DIR}/${source_file}" ]]; then
    missing_sources+=("${source_file}")
  fi
done
if (( ${#missing_sources[@]} > 0 )); then
  echo "FFmpeg source tree is incomplete: ${SRC_DIR}" >&2
  printf 'Missing required file: %s\n' "${missing_sources[@]}" >&2
  echo "Commit the full third_party/FFmpeg source tree to GitHub, including Makefile and ffbuild/*.mak." >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}" "${PREFIX}" "${ROOT_DIR}/include" "${ROOT_DIR}/lib"

cd "${BUILD_DIR}"

if [[ ! -f config.mak ]]; then
  extra_configure_args=()
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists x264; then
    extra_configure_args+=(--enable-gpl --enable-libx264 --enable-encoder=libx264)
  else
    extra_configure_args+=(--enable-encoder=h264_v4l2m2m)
  fi

  "${SRC_DIR}/configure" \
    --prefix="${PREFIX}" \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-autodetect \
    --disable-asm \
    --disable-x86asm \
    --enable-avdevice \
    --enable-swscale \
    --enable-swresample \
    --enable-network \
    --disable-everything \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-avdevice \
    --enable-protocol=file,pipe,tcp,udp,rtmp,rtmpt,rtsp,http,rtp \
    --enable-demuxer=mov,mp4,m4a,3gp,3g2,mj2,flv,rtsp,rtp,mpegts,h264,hevc,aac,matroska \
    --enable-muxer=flv,rtsp,rtp,mpegts,mp4,null \
    --enable-indev=v4l2 \
    --enable-decoder=h264,mjpeg,rawvideo \
    --enable-parser=h264,hevc,aac,mpeg4video,mjpeg \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
    --enable-pic \
    "${extra_configure_args[@]}"
fi

make -j"$(nproc)"
make install

cp -a "${PREFIX}/include/"* "${ROOT_DIR}/include/"
cp -a "${PREFIX}/lib/"libav*.so* "${ROOT_DIR}/lib/"
cp -a "${PREFIX}/lib/"libsw*.so* "${ROOT_DIR}/lib/" 2>/dev/null || true

cd "${ROOT_DIR}/lib"
for lib in libavcodec libavdevice libavfilter libavformat libavutil libswresample libswscale; do
  if [[ -e "${lib}.so" ]]; then
    soname="$(readelf -d "${lib}.so" 2>/dev/null | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p' | head -1)"
    if [[ -n "${soname}" && "${soname}" != "${lib}.so" ]]; then
      ln -sf "${lib}.so" "${soname}"
    fi
  fi
done

echo "FFmpeg SDK built into:"
echo "  ${ROOT_DIR}/include"
echo "  ${ROOT_DIR}/lib"
