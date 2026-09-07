// RUN: schema-opt %s | schema-opt | FileCheck %s

// CHECK-LABEL: func.func @person
func.func @person(%doc: !schema.value) -> i1 {
  // CHECK: schema.validate_string %{{.*}} {max_length = 64 : i64, min_length = 1 : i64, pattern = "^[A-Za-z ]+$"} : !schema.value
  %name = schema.validate_string %doc {
    min_length = 1 : i64, max_length = 64 : i64, pattern = "^[A-Za-z ]+$"
  } : !schema.value

  // CHECK: schema.validate_number %{{.*}} {integral, maximum = 1.500000e+02 : f64, minimum = 0.000000e+00 : f64} : !schema.value
  %age = schema.validate_number %doc {
    minimum = 0.0 : f64, maximum = 150.0 : f64, integral
  } : !schema.value

  // CHECK: schema.struct %{{.*}} as "Person" fields ["name", "age"] required ["name"] validators(%{{.*}}, %{{.*}}) : (!schema.value, i1, i1) -> i1
  %ok = schema.struct %doc as "Person"
          fields ["name", "age"] required ["name"]
          validators(%name, %age)
        : (!schema.value, i1, i1) -> i1

  return %ok : i1
}