# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Added

- Mojo library (`mojo/schema/`) with the canonicalizer's constraint lattices
  and the lowered validation semantics, plus tests that check `meet` never
  changes a verdict.
- `examples/person/`: a JSON Schema, its `schema` dialect IR, and the
  canonicalized output.
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
