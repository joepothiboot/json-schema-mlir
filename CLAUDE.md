# json-schema-mlir

Out-of-tree MLIR dialect (`schema`) that compiles JSON Schema into native validators.
See `README.md` for the full pipeline, runtime ABI and pass reference.

## Layout

- `include/Schema/` — ODS (`SchemaDialect.td`, `SchemaOps.td`), headers, pass decls
- `lib/Schema/` — dialect/ops impl, `SchemaCanonicalizerPass.cpp`, `LowerToStandard.cpp`
- `tools/schema-opt/` — `mlir-opt`-style driver
- `test/` — lit + FileCheck tests (`Dialect/Schema/`, `Lowering/`)

## Pipeline (short)

`schema.validate_string` / `schema.validate_number` / `schema.struct` on `!schema.value`
→ `--schema-canonicalize` (subsumption, fusion, vacuous elimination)
→ `--lower-schema-to-std` (`arith`/`scf`/`math` + `func.call @__schema_rt_*`)
→ `--schema-to-llvm-pipeline` → LLVM IR.

## Build & test

```bash
./build.sh                       # configure + build schema-opt + run check-schema
ninja -C build check-schema      # re-run tests after the first configure
```

Requires LLVM/MLIR 19+ (verified on 23.1.1); `MLIR_INSTALL` defaults to `brew --prefix llvm`.

## Conventions

- LLVM is built without RTTI: use `llvm::dyn_cast` / `TypeSwitch`, never `dynamic_cast`.
- Every behavior change gets a lit test; keep `CHECK` lines tight.
- Format with Prettier (non-C++) and match the existing LLVM style in C++.
