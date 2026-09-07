# json-schema-mlir

An out-of-tree MLIR dialect for expressing JSON Schema validation rules.

## Layout

- `include/Schema`: dialect, type, operation, and TableGen definitions
- `lib/Schema`: dialect registration and operation verifiers
- `tools/schema-opt`: an `mlir-opt`-style driver
- `test/Dialect/Schema`: lit regression tests

## Build

Set `PREFIX` to an LLVM/MLIR installation containing `llvm`, `mlir`, and
`lit`, then run:

```sh
./build.sh
```

The optimizer is written to `build/bin/schema-opt`. Tests can also be run with
`cmake --build build --target check-schema`.

## Usage

Run the optimizer on the example dialect program:

```sh
build/bin/schema-opt test/Dialect/Schema/ops.mlir
```

The input contains string and number validators followed by an object
validator:

```mlir
%name = schema.validate_string %doc {
	min_length = 1 : i64, max_length = 64 : i64
} : !schema.value

%age = schema.validate_number %doc {
	minimum = 0.0 : f64, maximum = 150.0 : f64, integral
} : !schema.value
```

`schema-opt` parses and prints normalized MLIR. For example, attributes are
printed in a stable order:

```mlir
schema.validate_string %doc {
	max_length = 64 : i64, min_length = 1 : i64
} : !schema.value
```

Run the regression tests with:

```sh
cmake --build build --target check-schema
```
