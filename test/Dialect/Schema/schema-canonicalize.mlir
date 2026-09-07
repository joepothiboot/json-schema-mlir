// RUN: schema-opt %s --schema-canonicalize --split-input-file | FileCheck %s

// A stricter `min_length` subsumes a weaker one under conjunction: the weak
// validation is absorbed and its defining op becomes trivially dead.

// CHECK-LABEL: func.func @redundant_min_length
// CHECK-SAME:      %[[DOC:.*]]: !schema.value
//       CHECK:    %[[V:.*]] = schema.validate_string %[[DOC]] {min_length = 5 : i64} : !schema.value
//       CHECK:    return %[[V]] : i1
//   CHECK-NOT:    min_length = 2
//   CHECK-NOT:    arith.andi
func.func @redundant_min_length(%doc: !schema.value) -> i1 {
  %strong = schema.validate_string %doc { min_length = 5 : i64 } : !schema.value
  %weak   = schema.validate_string %doc { min_length = 2 : i64 } : !schema.value
  %ok = arith.andi %strong, %weak : i1
  return %ok : i1
}

// -----

// Non-comparable constraints on the same input are *fused* into a single
// operation carrying the meet of the lattice, collapsing the whole `andi` tree.

// CHECK-LABEL: func.func @fuse_constraint_tree
// CHECK-SAME:      %[[DOC:.*]]: !schema.value
//       CHECK:    %[[V:.*]] = schema.validate_string %[[DOC]] {max_length = 32 : i64, min_length = 5 : i64, pattern = "^[a-z]+$"} : !schema.value
//       CHECK:    return %[[V]] : i1
//   CHECK-NOT:    arith.andi
func.func @fuse_constraint_tree(%doc: !schema.value) -> i1 {
  %a = schema.validate_string %doc { min_length = 5 : i64 } : !schema.value
  %b = schema.validate_string %doc { max_length = 32 : i64 } : !schema.value
  %c = schema.validate_string %doc { min_length = 2 : i64, pattern = "^[a-z]+$" } : !schema.value
  %ab  = arith.andi %a, %b : i1
  %abc = arith.andi %ab, %c : i1
  return %abc : i1
}

// -----

// Numeric bounds: `minimum = 10.0` subsumes `minimum = 0.0`, and the exclusive
// form of an equal bound subsumes the inclusive form.

// CHECK-LABEL: func.func @redundant_numeric_bounds
// CHECK-SAME:      %[[DOC:.*]]: !schema.value
//       CHECK:    %[[V:.*]] = schema.validate_number %[[DOC]] {integral, minimum = 1.000000e+01 : f64} : !schema.value
//       CHECK:    return %[[V]] : i1
//   CHECK-NOT:    minimum = 0.000000e+00
//   CHECK-NOT:    arith.andi
func.func @redundant_numeric_bounds(%doc: !schema.value) -> i1 {
  %strong = schema.validate_number %doc { minimum = 1.000000e+01 : f64, integral } : !schema.value
  %weak   = schema.validate_number %doc { minimum = 0.000000e+00 : f64 } : !schema.value
  %ok = arith.andi %strong, %weak : i1
  return %ok : i1
}

// -----

// `schema.struct` reduces its validator operands conjunctively, so a subsumed
// operand is rewritten to the stronger validator and the weak op is DCE'd.

// CHECK-LABEL: func.func @struct_subsumption
// CHECK-SAME:      %[[DOC:.*]]: !schema.value
//       CHECK:    %[[V:.*]] = schema.validate_string %[[DOC]] {min_length = 8 : i64} : !schema.value
//       CHECK:    schema.struct %[[DOC]] as "Token" fields ["id", "alias"] validators(%[[V]], %[[V]])
//   CHECK-NOT:    min_length = 3
func.func @struct_subsumption(%doc: !schema.value) -> i1 {
  %strong = schema.validate_string %doc { min_length = 8 : i64 } : !schema.value
  %weak   = schema.validate_string %doc { min_length = 3 : i64 } : !schema.value
  %ok = schema.struct %doc as "Token"
          fields ["id", "alias"]
          validators(%strong, %weak)
        : (!schema.value, i1, i1) -> i1
  return %ok : i1
}

// -----

// Negative test: regular-expression containment is not decided, so two
// distinct patterns have no representable meet and both ops must survive.

// CHECK-LABEL: func.func @no_merge_conflicting_patterns
//       CHECK:    schema.validate_string %{{.*}} {pattern = "^a+$"}
//       CHECK:    schema.validate_string %{{.*}} {pattern = "^b+$"}
//       CHECK:    arith.andi
func.func @no_merge_conflicting_patterns(%doc: !schema.value) -> i1 {
  %a = schema.validate_string %doc { pattern = "^a+$" } : !schema.value
  %b = schema.validate_string %doc { pattern = "^b+$" } : !schema.value
  %ok = arith.andi %a, %b : i1
  return %ok : i1
}

// -----

// Negative test: different SSA inputs are never merged.

// CHECK-LABEL: func.func @distinct_inputs
//       CHECK:    schema.validate_string %arg0
//       CHECK:    schema.validate_string %arg1
//       CHECK:    arith.andi
func.func @distinct_inputs(%lhs: !schema.value, %rhs: !schema.value) -> i1 {
  %a = schema.validate_string %lhs { min_length = 5 : i64 } : !schema.value
  %b = schema.validate_string %rhs { min_length = 2 : i64 } : !schema.value
  %ok = arith.andi %a, %b : i1
  return %ok : i1
}

// -----

// Local normalization: `min_length = 0` is vacuous, `multiple_of = 1.0` is
// implied by `integral`.

// CHECK-LABEL: func.func @normalize_vacuous
//       CHECK:    schema.validate_string %{{.*}} {max_length = 4 : i64} : !schema.value
//       CHECK:    schema.validate_number %{{.*}} {integral} : !schema.value
//   CHECK-NOT:    min_length = 0
//   CHECK-NOT:    multiple_of
func.func @normalize_vacuous(%doc: !schema.value) -> (i1, i1) {
  %s = schema.validate_string %doc { min_length = 0 : i64, max_length = 4 : i64 } : !schema.value
  %n = schema.validate_number %doc { multiple_of = 1.000000e+00 : f64, integral } : !schema.value
  return %s, %n : i1, i1
}