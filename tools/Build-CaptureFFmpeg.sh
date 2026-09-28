#!/bin/sh
# Run in an unpacked, unmodified FFmpeg 9.0.2 source directory.
# Requires w64devkit 2.10.0 (x64) and NASM 2.16.03 on PATH.
set -eu

./configure \
    --target-os=mingw32 \
    --arch=x86_64 \
    --disable-autodetect \
    --disable-everything \
    --disable-network \
    --disable-doc \
    --disable-debug \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-avdevice \
    --disable-swresample \
    --disable-shared \
    --enable-static \
    --enable-ffmpeg \
    --enable-encoder=prores_ks \
    --enable-decoder=rawvideo \
    --enable-demuxer=rawvideo \
    --enable-muxer=mov \
    --enable-protocol=file,pipe,fd \
    --enable-filter=buffer,buffersink,scale,format,null,trim \
    --extra-cflags=-O3 \
    --extra-ldflags=-static

make V=1 -j "${JOBS:-8}" ffmpeg.exe
