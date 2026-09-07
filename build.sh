cmake -G Ninja -B build \
  -DMLIR_DIR=$PREFIX/lib/cmake/mlir \
  -DLLVM_DIR=$PREFIX/lib/cmake/llvm \
  -DLLVM_EXTERNAL_LIT=$(which lit) \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target schema-opt check-schema
