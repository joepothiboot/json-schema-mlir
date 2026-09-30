module {
  func.func @validate_person(%arg0: !schema.value, %arg1: !schema.value, %arg2: !schema.value) -> i1 {
    %0 = schema.validate_string %arg1 : !schema.value
    %1 = schema.validate_string %arg1 {min_length = 1 : i64} : !schema.value
    %2 = schema.validate_string %arg1 {max_length = 64 : i64} : !schema.value
    %3 = arith.andi %0, %1 : i1
    %4 = arith.andi %3, %2 : i1
    %5 = schema.validate_number %arg2 {integral} : !schema.value
    %6 = schema.validate_number %arg2 {minimum = 0.000000e+00 : f64} : !schema.value
    %7 = arith.andi %5, %6 : i1
    %8 = schema.validate_number %arg2 : !schema.value
    %9 = schema.validate_number %arg2 {exclusive_maximum, maximum = 1.500000e+02 : f64} : !schema.value
    %10 = arith.andi %8, %9 : i1
    %11 = schema.validate_number %arg2 : !schema.value
    %12 = schema.validate_number %arg2 {minimum = 1.800000e+01 : f64} : !schema.value
    %13 = arith.andi %11, %12 : i1
    %14 = arith.andi %7, %10 : i1
    %15 = arith.andi %14, %13 : i1
    %16 = schema.struct %arg0 as "Person" fields ["name", "age"] required ["name"] validators(%4, %15) : (!schema.value, i1, i1) -> i1
    return %16 : i1
  }
}
