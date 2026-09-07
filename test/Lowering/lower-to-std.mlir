// RUN: schema-opt %s --lower-schema-to-std --split-input-file | FileCheck %s
// RUN: schema-opt %s --lower-schema-to-std --split-input-file | schema-opt | FileCheck %s
// RUN: schema-opt %s --schema-to-llvm-pipeline --split-input-file | \
// RUN:   FileCheck %s --check-prefix=LLVM

// Runtime ABI declarations are materialized once, before conversion.
// CHECK-DAG: func.func private @__schema_rt_kind(i64) -> i32
// CHECK-DAG: func.func private @__schema_rt_as_f64(i64) -> f64

// The opaque handle type is converted in the function signature.
// CHECK-LABEL: func.func @bounded_number
// CHECK-SAME:      %[[DOC:.*]]: i64
// CHECK-SAME:      -> i1
//   CHECK-DAG:   %[[LO:.*]] = arith.constant 0.000000e+00 : f64
//   CHECK-DAG:   %[[HI:.*]] = arith.constant 1.500000e+02 : f64
//   CHECK-DAG:   %[[NUMTAG:.*]] = arith.constant 2 : i32
//       CHECK:   %[[KIND:.*]] = func.call @__schema_rt_kind(%[[DOC]]) : (i64) -> i32
//       CHECK:   %[[ISNUM:.*]] = arith.cmpi eq, %[[KIND]], %[[NUMTAG]] : i32
//       CHECK:   %[[R:.*]] = scf.if %[[ISNUM]] -> (i1) {
//       CHECK:     %[[X:.*]] = func.call @__schema_rt_as_f64(%[[DOC]]) : (i64) -> f64
//       CHECK:     %[[GE:.*]] = arith.cmpf oge, %[[X]], %[[LO]] : f64
//       CHECK:     %[[LE:.*]] = arith.cmpf ole, %[[X]], %[[HI]] : f64
//       CHECK:     %[[AND:.*]] = arith.andi %[[GE]], %[[LE]] : i1
//       CHECK:     scf.yield %[[AND]] : i1
//       CHECK:   } else {
//       CHECK:     %[[F:.*]] = arith.constant false
//       CHECK:     scf.yield %[[F]] : i1
//       CHECK:   }
//       CHECK:   return %[[R]] : i1
//   CHECK-NOT:   schema.
//   CHECK-NOT:   !schema.value
func.func @bounded_number(%doc: !schema.value) -> i1 {
  %ok = schema.validate_number %doc {
    minimum = 0.000000e+00 : f64, maximum = 1.500000e+02 : f64
  } : !schema.value
  return %ok : i1
}

// -----

// Exclusive bounds select the strict floating-point predicates.

// CHECK-LABEL: func.func @exclusive_bounds
//       CHECK:   arith.cmpf ogt, %{{.*}}, %{{.*}} : f64
//       CHECK:   arith.cmpf olt, %{{.*}}, %{{.*}} : f64
//   CHECK-NOT:   arith.cmpf oge
//   CHECK-NOT:   arith.cmpf ole
func.func @exclusive_bounds(%doc: !schema.value) -> i1 {
  %ok = schema.validate_number %doc {
    minimum = 1.000000e+00 : f64, maximum = 1.000000e+01 : f64,
    exclusive_minimum, exclusive_maximum
  } : !schema.value
  return %ok : i1
}

// -----

// `integral` becomes `x == trunc(x)`; `multiple_of` becomes an exact-quotient
// test against `roundeven`.

// CHECK-LABEL: func.func @integral_and_multiple_of
//       CHECK:   %[[X:.*]] = func.call @__schema_rt_as_f64
//       CHECK:   %[[Q:.*]] = arith.divf %[[X]], %{{.*}} : f64
//       CHECK:   %[[RE:.*]] = math.roundeven %[[Q]] : f64
//       CHECK:   arith.cmpf oeq, %[[Q]], %[[RE]] : f64
//       CHECK:   %[[T:.*]] = math.trunc %[[X]] : f64
//       CHECK:   arith.cmpf oeq, %[[X]], %[[T]] : f64
func.func @integral_and_multiple_of(%doc: !schema.value) -> i1 {
  %ok = schema.validate_number %doc {
    multiple_of = 5.000000e+00 : f64, integral
  } : !schema.value
  return %ok : i1
}

// -----

// An unconstrained number assertion degenerates to a pure type check.

// CHECK-LABEL: func.func @type_check_only
//       CHECK:   arith.cmpi eq, %{{.*}}, %{{.*}} : i32
//       CHECK:   scf.if
//       CHECK:     %[[T:.*]] = arith.constant true
//       CHECK:     scf.yield %[[T]] : i1
//   CHECK-NOT:   arith.cmpf
func.func @type_check_only(%doc: !schema.value) -> i1 {
  %ok = schema.validate_number %doc : !schema.value
  return %ok : i1
}

// -----

// End-to-end object validation: string + number + struct all disappear, and
// the interned literals land in the module-level string pool.

// CHECK: module attributes {schema.string_pool = ["^[A-Za-z ]+$", "name"]}
// CHECK-LABEL: func.func @person
// CHECK-SAME:      %[[DOC:.*]]: i64
//   CHECK-DAG:   func.call @__schema_rt_str_len(%[[DOC]]) : (i64) -> i64
//   CHECK-DAG:   func.call @__schema_rt_str_matches(%[[DOC]], %{{.*}}) : (i64, i64) -> i1
//   CHECK-DAG:   func.call @__schema_rt_has_field(%[[DOC]], %{{.*}}) : (i64, i64) -> i1
//       CHECK:   return %{{.*}} : i1
//   CHECK-NOT:   schema.
func.func @person(%doc: !schema.value) -> i1 {
  %name = schema.validate_string %doc {
    min_length = 1 : i64, pattern = "^[A-Za-z ]+$"
  } : !schema.value
  %age = schema.validate_number %doc {
    minimum = 0.000000e+00 : f64, integral
  } : !schema.value
  %ok = schema.struct %doc as "Person"
          fields ["name", "age"] required ["name"]
          validators(%name, %age)
        : (!schema.value, i1, i1) -> i1
  return %ok : i1
}

// -----

// The full pipeline reaches the LLVM dialect with no residual casts.

// LLVM-LABEL: llvm.func @llvm_ready
//       LLVM:   llvm.call @__schema_rt_kind
//       LLVM:   llvm.fcmp
//       LLVM:   llvm.return
//   LLVM-NOT:   builtin.unrealized_conversion_cast
//   LLVM-NOT:   schema.
func.func @llvm_ready(%doc: !schema.value) -> i1 {
  %ok = schema.validate_number %doc {
    minimum = 0.000000e+00 : f64
  } : !schema.value
  return %ok : i1
}