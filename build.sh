#!/usr/bin/env bash
set -euo pipefail

MLIR_INSTALL="${MLIR_INSTALL:-${PREFIX:-}}"
if [[ -z "$MLIR_INSTALL" ]] && command -v brew >/dev/null 2>&1; then
  MLIR_INSTALL="$(brew --prefix llvm 2>/dev/null || true)"
fi
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
if [[ -z "$LLVM_LIT" ]] && command -v python3 >/dev/null 2>&1; then
  PYTHON_USER_LIT="$(python3 -m site --user-base 2>/dev/null || true)/bin/lit"
  if [[ -x "$PYTHON_USER_LIT" ]]; then
    LLVM_LIT="$PYTHON_USER_LIT"
  fi
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
