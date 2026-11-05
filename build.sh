#!/usr/bin/env bash
#
# Copyright (C) 2018 The Android Open Source Project
# Copyright (C) 2026 Xynes / Redroid C2 Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NDK_DIR="${ANDROID_NDK_ROOT:-$HOME/.local/share/android-ndk/android-ndk-r25c}"
if [ ! -d "$NDK_DIR" ]; then
    echo "Error: Android NDK not found at $NDK_DIR"
    echo "Set ANDROID_NDK_ROOT or install NDK r25c."
    exit 1
fi

CLANG64="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android31-clang++"
CLANG32="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android31-clang++"
STRIP="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

CXXFLAGS="-std=c++17 -fno-rtti -fPIC -O2 -D_LIBCPP_ABI_NAMESPACE=__1 -I$SCRIPT_DIR -I$SCRIPT_DIR/include -I$SCRIPT_DIR/include/media -I$SCRIPT_DIR/vaapi -I$SCRIPT_DIR/core"

mkdir -p out/lib out/lib64 out/bin

echo "[1/4] Building 64-bit libcodec2_redroid_vaapi.so..."
$CLANG64 $CXXFLAGS -c core/SimpleC2Component.cpp -o core/SimpleC2Component.64.o
$CLANG64 $CXXFLAGS -c core/SimpleC2Interface.cpp -o core/SimpleC2Interface.64.o
$CLANG64 $CXXFLAGS -c vaapi/vaapi_wrapper.cpp -o vaapi/vaapi_wrapper.64.o
$CLANG64 $CXXFLAGS -c core/C2RedroidEnc.cpp -o core/C2RedroidEnc.64.o
$CLANG64 $CXXFLAGS -c core/C2RedroidAvcEnc.cpp -o core/C2RedroidAvcEnc.64.o
$CLANG64 $CXXFLAGS -c core/C2RedroidHevcEnc.cpp -o core/C2RedroidHevcEnc.64.o
$CLANG64 $CXXFLAGS -c core/C2RedroidComponentStore.cpp -o core/C2RedroidComponentStore.64.o

$CLANG64 -shared -nostdlib++ -o out/lib64/libcodec2_redroid_vaapi.so \
  core/SimpleC2Component.64.o \
  core/SimpleC2Interface.64.o \
  vaapi/vaapi_wrapper.64.o \
  core/C2RedroidEnc.64.o \
  core/C2RedroidAvcEnc.64.o \
  core/C2RedroidHevcEnc.64.o \
  core/C2RedroidComponentStore.64.o \
  -L"$SCRIPT_DIR/sysroot/lib64" \
  -lcodec2 -lcodec2_vndk -lva -lva-drm -llog -lcutils -lutils -lui -ldrm -lc++

echo "[2/4] Building 32-bit libcodec2_redroid_vaapi.so..."
$CLANG32 $CXXFLAGS -c core/SimpleC2Component.cpp -o core/SimpleC2Component.32.o
$CLANG32 $CXXFLAGS -c core/SimpleC2Interface.cpp -o core/SimpleC2Interface.32.o
$CLANG32 $CXXFLAGS -c vaapi/vaapi_wrapper.cpp -o vaapi/vaapi_wrapper.32.o
$CLANG32 $CXXFLAGS -c core/C2RedroidEnc.cpp -o core/C2RedroidEnc.32.o
$CLANG32 $CXXFLAGS -c core/C2RedroidAvcEnc.cpp -o core/C2RedroidAvcEnc.32.o
$CLANG32 $CXXFLAGS -c core/C2RedroidHevcEnc.cpp -o core/C2RedroidHevcEnc.32.o
$CLANG32 $CXXFLAGS -c core/C2RedroidComponentStore.cpp -o core/C2RedroidComponentStore.32.o

$CLANG32 -shared -nostdlib++ -o out/lib/libcodec2_redroid_vaapi.so \
  core/SimpleC2Component.32.o \
  core/SimpleC2Interface.32.o \
  vaapi/vaapi_wrapper.32.o \
  core/C2RedroidEnc.32.o \
  core/C2RedroidAvcEnc.32.o \
  core/C2RedroidHevcEnc.32.o \
  core/C2RedroidComponentStore.32.o \
  -L"$SCRIPT_DIR/sysroot/lib" \
  -lcodec2 -lcodec2_vndk -lva -lva-drm -llog -lcutils -lutils -lui -ldrm -lc++

echo "[3/4] Building 64-bit HAL daemon android.hardware.media.c2@1.2-service.redroid..."
$CLANG64 $CXXFLAGS -c service/service.cpp -o service/service.64.o

$CLANG64 -nostdlib++ \
  service/service.64.o \
  -L"$SCRIPT_DIR/out/lib64" \
  -lcodec2_redroid_vaapi \
  -L"$SCRIPT_DIR/sysroot/lib64" \
  -lcodec2_hidl@1.2 \
  -l:android.hardware.media.c2@1.2.so \
  -l:android.hardware.media.c2@1.1.so \
  -l:android.hardware.media.c2@1.0.so \
  -l:android.hardware.media.bufferpool@2.0.so \
  -lhidlbase \
  -llog \
  -lcutils \
  -lutils \
  -lcodec2 \
  -lcodec2_vndk \
  -lc++ \
  -o out/bin/android.hardware.media.c2@1.2-service.redroid

echo "[4/4] Build complete!"
ls -lh out/lib64/libcodec2_redroid_vaapi.so out/lib/libcodec2_redroid_vaapi.so out/bin/android.hardware.media.c2@1.2-service.redroid
