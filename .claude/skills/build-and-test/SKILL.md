---
name: build-and-test
description: Build schema-opt and run the lit regression suite for this MLIR project. Use after changing .td, C++ or .mlir test files, or when asked to build, test or inspect a pass.
---

# Build and test

1. First build (or after CMake changes): `./build.sh`
2. Incremental: `ninja -C build check-schema`
3. One test, verbose: `"$MLIR_INSTALL/bin/llvm-lit" -v --filter=<name> build/test`
   (`MLIR_INSTALL` is usually `$(brew --prefix llvm)`)

Inspect a pass by hand:

```bash
./build/bin/schema-opt test/Lowering/lower-to-std.mlir --lower-schema-to-std --split-input-file
```

If a test fails, read the FileCheck diff, fix the code (or the `CHECK` line if the new
output is intended), and re-run until the suite is green. Report the final pass/fail count.
