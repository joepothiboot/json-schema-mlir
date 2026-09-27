# Person

`person.schema.json` and the same schema written in the `schema` dialect
(`person.mlir`). The three `allOf` branches on `age` start as three ops
joined with `arith.andi`:

```bash
build/bin/schema-opt examples/person/person.mlir --schema-canonicalize
```

`--schema-canonicalize` fuses them into one op carrying their meet
(`minimum = 18` subsumes `minimum = 0`):

```mlir
module {
  func.func @validate_person(%arg0: !schema.value, %arg1: !schema.value, %arg2: !schema.value) -> i1 {
    %0 = schema.validate_string %arg1 {max_length = 64 : i64, min_length = 1 : i64} : !schema.value
    %1 = schema.validate_number %arg2 {exclusive_maximum, integral, maximum = 1.500000e+02 : f64, minimum = 1.800000e+01 : f64} : !schema.value
    %2 = schema.struct %arg0 as "Person" fields ["name", "age"] required ["name"] validators(%0, %1) : (!schema.value, i1, i1) -> i1
    return %2 : i1
  }
}
```

`mojo/tests/test_lattice.mojo` (`test_person_example`) computes the same
meet with the Mojo library and checks it gives the same verdicts as the
three separate constraints.
