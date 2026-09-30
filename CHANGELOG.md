# Changelog 📜

All notable changes to this project are documented here.

## [Unreleased]

### Added

- JSON Schema front end: `schema-translate --import-json-schema` lexes,
  parses and imports a Draft 2020-12 schema (`type`, `properties`,
  `required`, `allOf`, numeric bounds, `multipleOf`, `minLength`/`maxLength`,
  `pattern`, `format`) into the `schema` dialect, with one op per keyword
  located at that keyword. Unsupported keywords and semantic errors are
  located diagnostics.
- `--dump-tokens` and `--dump-ast` to print the lexer and parser stages, and
  `--allow-comments` for JSONC input.
- `--emit-trace=<file.json>`: tokens, AST, diagnostics and IR before and
  after `--schema-canonicalize`, tied to source ranges. Format in
  `docs/trace-format.md` (version 1).
- `test/Import/`: token, AST, IR, diagnostic and trace tests.
- Mojo library (`mojo/schema/`) with the canonicalizer's constraint lattices
  and the lowered validation semantics, plus tests that check `meet` never
  changes a verdict.
- `examples/person/`: a JSON Schema, its imported `schema` dialect IR, and
  the canonicalized output.
- `pixi.toml` for the Mojo toolchain.

## [0.1.0] - 2026-09-07

### Added

- JSON Schema MLIR dialect operations and verifiers.
- Constraint canonicalization for redundant and subsumed validations.
- Lowering from `schema` to `arith`, `scf`, `math`, and `func`.
- Optional lowering pipeline from `schema` to the LLVM dialect.
- Runtime ABI declarations and string-pool interning for lowered validators.
- Lit/FileCheck tests for canonicalization and lowering.
- Build, usage, pipeline, and runtime ABI documentation.
