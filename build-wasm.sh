#!/bin/sh
SRCDIR=$(dirname "$0")
make ARCH=wasm \
  LLVM=1 \
  HOSTCC=gcc \
  CC=$HOME/wasi-sdk/bin/clang \
  LD=$SRCDIR/arch/wasm/wasm-ld-wrapper.sh \
  AR=$HOME/wasi-sdk/bin/llvm-ar \
  NM=$HOME/wasi-sdk/bin/llvm-nm \
  OBJCOPY=$HOME/wasi-sdk/bin/llvm-objcopy \
  OBJDUMP=$HOME/wasi-sdk/bin/llvm-objdump \
  STRIP=$HOME/wasi-sdk/bin/llvm-strip \
  READELF=$HOME/wasi-sdk/bin/llvm-readelf \
  "$@"
