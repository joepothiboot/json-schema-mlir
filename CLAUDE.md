# json-schema-mlir

Out-of-tree MLIR dialect (`schema`) that compiles JSON Schema into native validators.
See `README.md` for the full pipeline, runtime ABI and pass reference.

## Layout

- `include/Schema/` — ODS (`SchemaDialect.td`, `SchemaOps.td`), headers, pass decls
- `lib/Schema/` — dialect/ops impl, `SchemaCanonicalizerPass.cpp`, `LowerToStandard.cpp`
- `include/Schema/Import/`, `lib/Schema/Import/` — JSON Schema front end
  (`MLIRSchemaImport`): hand-written lexer, recursive-descent parser → AST,
  importer → `schema` ops located at their JSON keyword, `--emit-trace`
  writer (format in `docs/trace-format.md`)
- `tools/schema-opt/` — `mlir-opt`-style driver
- `tools/schema-translate/` — `mlir-translate`-style driver:
  `--import-json-schema` with `--dump-tokens`, `--dump-ast`,
  `--emit-trace=<file>`, `--allow-comments`
- `test/` — lit + FileCheck tests (`Dialect/Schema/`, `Lowering/`, `Import/`);
  JSON inputs live in `.test` files unpacked with `split-file`
- `mojo/schema/` — Mojo library: `StringConstraints` / `NumberConstraints`
  with `subsumes`/`meet` (same rules as `SchemaCanonicalizerPass.cpp`) and
  `validate` (same semantics as `LowerToStandard.cpp`)
- `mojo/tests/` — mirrors `schema-canonicalize.mlir` + a grid test that
  `meet` never changes a verdict
- `examples/` — JSON Schema + importer-generated `schema` IR pairs

## Pipeline (short)

`schema.json` → `schema-translate --import-json-schema` (one op per keyword)
→ `schema.validate_string` / `schema.validate_number` / `schema.struct` on `!schema.value`
→ `--schema-canonicalize` (subsumption, fusion, vacuous elimination)
→ `--lower-schema-to-std` (`arith`/`scf`/`math` + `func.call @__schema_rt_*`)
→ `--schema-to-llvm-pipeline` → LLVM IR.

## Build & test

```bash
./build.sh                       # configure + build schema-opt + run check-schema
ninja -C build check-schema      # re-run tests after the first configure
pixi run test-mojo               # Mojo lattice tests (installs Mojo 1.1 via pixi)
```

Requires LLVM/MLIR 19+ (verified on 23.1.1); `MLIR_INSTALL` defaults to `brew --prefix llvm`.

## Conventions

- LLVM is built without RTTI: use `llvm::dyn_cast` / `TypeSwitch`, never `dynamic_cast`.
- Every behavior change gets a lit test; keep `CHECK` lines tight.
- Front-end diagnostic tests use `--allow-comments -verify-diagnostics` so the
  JSON can carry `// expected-error` lines.
- lit cannot run from a path containing spaces (paths are substituted unquoted).
- A change to lattice rules or lowered semantics must land in the Mojo library
  (`mojo/schema/lattice.mojo`) in the same commit, with a matching test.
- Format with Prettier (non-C++) and match the existing LLVM style in C++.
