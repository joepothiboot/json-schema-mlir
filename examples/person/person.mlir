// person.schema.json, written by hand in the `schema` dialect (there is no
// JSON front end yet). The three `allOf` branches on `age` are separate ops
// joined with `arith.andi`, exactly as a naive front end would emit them.
//
//   schema-opt examples/person/person.mlir --schema-canonicalize
//   schema-opt examples/person/person.mlir --schema-to-llvm-pipeline

func.func @validate_person(%doc: !schema.value, %name: !schema.value,
                           %age: !schema.value) -> i1 {
  %name_ok = schema.validate_string %name {
    min_length = 1 : i64, max_length = 64 : i64
  } : !schema.value

  %age_int   = schema.validate_number %age { minimum = 0.0 : f64, integral } : !schema.value
  %age_upper = schema.validate_number %age { maximum = 150.0 : f64, exclusive_maximum } : !schema.value
  %age_adult = schema.validate_number %age { minimum = 18.0 : f64 } : !schema.value
  %a = arith.andi %age_int, %age_upper : i1
  %age_ok = arith.andi %a, %age_adult : i1

  %ok = schema.struct %doc as "Person"
          fields ["name", "age"] required ["name"]
          validators(%name_ok, %age_ok)
        : (!schema.value, i1, i1) -> i1
  return %ok : i1
}
