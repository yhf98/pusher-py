./configure \
  --prefix=/usr/local/ffmpeg \
  --enable-gpl \
  --enable-nonfree \
  \
  --enable-cuda \
  --enable-cuvid \
  --enable-nvenc \
  --enable-libnpp \
  \
  --extra-cflags=-I/usr/local/cuda/include \
  --extra-ldflags=-L/usr/local/cuda/lib64 \
  \
  --enable-libx264 \
  --enable-libx265 \
  --enable-libvpx \
  --enable-libfdk-aac \
  --enable-libmp3lame \
  --enable-libopus \
  \
  --enable-libass \
  --enable-libfreetype \
  --enable-libfontconfig \
  --enable-libfribidi \
  \
  --enable-openssl \
  \
  --enable-shared \
  --disable-static