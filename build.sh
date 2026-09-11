#!/usr/bin/env bash
set -euo pipefail

MLIR_INSTALL="${MLIR_INSTALL:-${PREFIX:-}}"
if [[ -z "$MLIR_INSTALL" ]]; then
  printf 'Set MLIR_INSTALL to the LLVM/MLIR installation prefix.\n' >&2
  exit 1
fi

if [[ ! -f "$MLIR_INSTALL/lib/cmake/llvm/LLVMConfig.cmake" ]]; then
  printf 'LLVMConfig.cmake not found under %s. Install LLVM/MLIR or correct MLIR_INSTALL.\n' "$MLIR_INSTALL" >&2
  exit 1
fi

LLVM_LIT="$MLIR_INSTALL/bin/llvm-lit"
if [[ ! -x "$LLVM_LIT" ]]; then
  LLVM_LIT="$(command -v lit || true)"
fi
if [[ -z "$LLVM_LIT" ]]; then
  printf 'Could not find llvm-lit or lit under %s.\n' "$MLIR_INSTALL" >&2
  exit 1
fi

cmake -G Ninja -B build \
  -DMLIR_DIR="$MLIR_INSTALL/lib/cmake/mlir" \
  -DLLVM_DIR="$MLIR_INSTALL/lib/cmake/llvm" \
  -DLLVM_EXTERNAL_LIT="$LLVM_LIT" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target schema-opt check-schema
