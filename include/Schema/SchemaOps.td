
---

## 3. `include/Schema/SchemaOps.td`

```tablegen
//===- SchemaOps.td - JSON Schema dialect operations -------*- tablegen -*-===//
#ifndef SCHEMA_OPS_TD
#define SCHEMA_OPS_TD

include "Schema/SchemaDialect.td"
include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

//===----------------------------------------------------------------------===//
// schema.validate_string
//===----------------------------------------------------------------------===//

def Schema_ValidateStringOp : Schema_Op<"validate_string", [Pure]> {
  let summary = "Validate a JSON value against string-typed constraints.";
  let description = [{
    Applies the JSON Schema string assertions (`minLength`, `maxLength`,
    `pattern`, `format`) to `$input`. Every attribute is optional; an absent
    attribute means the corresponding assertion is unconstrained. The op
    returns `false` when `$input` is not a JSON string.

    Example:
    ```mlir
    %ok = schema.validate_string %doc {
      min_length = 1 : i64, max_length = 64 : i64, pattern = "^[A-Za-z ]+$"
    } : !schema.value
    ```
  }];

  let arguments = (ins
    Schema_ValueType:$input,
    OptionalAttr<I64Attr>:$min_length,
    OptionalAttr<I64Attr>:$max_length,
    OptionalAttr<StrAttr>:$pattern,
    OptionalAttr<StrAttr>:$format
  );

  let results = (outs Schema_Verdict:$valid);

  // The `i1` result type is buildable, so ODS infers it and keeps the
  // printed form minimal.
  let assemblyFormat = [{ $input attr-dict `:` type($input) }];

  let builders = [
    OpBuilder<(ins "::mlir::Value":$input), [{
      build($_builder, $_state, $_builder.getI1Type(), input,
            /*min_length=*/nullptr, /*max_length=*/nullptr,
            /*pattern=*/nullptr, /*format=*/nullptr);
    }]>
  ];

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// schema.validate_number
//===----------------------------------------------------------------------===//

def Schema_ValidateNumberOp : Schema_Op<"validate_number", [Pure]> {
  let summary = "Validate a JSON value against numeric constraints.";
  let description = [{
    Applies the JSON Schema numeric assertions (`minimum`, `maximum`,
    `multipleOf`) to `$input`. The unit attributes `exclusive_minimum` and
    `exclusive_maximum` turn the corresponding bound into a strict comparison
    and require that bound to be present. `integral` restricts the value to
    the `integer` JSON type.

    Example:
    ```mlir
    %ok = schema.validate_number %doc {
      minimum = 0.0 : f64, maximum = 150.0 : f64, integral
    } : !schema.value
    ```
  }];

  let arguments = (ins
    Schema_ValueType:$input,
    OptionalAttr<F64Attr>:$minimum,
    OptionalAttr<F64Attr>:$maximum,
    OptionalAttr<F64Attr>:$multiple_of,
    UnitAttr:$exclusive_minimum,
    UnitAttr:$exclusive_maximum,
    UnitAttr:$integral
  );

  let results = (outs Schema_Verdict:$valid);

  let assemblyFormat = [{ $input attr-dict `:` type($input) }];

  let builders = [
    OpBuilder<(ins "::mlir::Value":$input), [{
      build($_builder, $_state, $_builder.getI1Type(), input,
            /*minimum=*/nullptr, /*maximum=*/nullptr, /*multiple_of=*/nullptr,
            /*exclusive_minimum=*/nullptr, /*exclusive_maximum=*/nullptr,
            /*integral=*/nullptr);
    }]>
  ];

  let hasVerifier = 1;
}

//===----------------------------------------------------------------------===//
// schema.struct
//===----------------------------------------------------------------------===//

def Schema_StructOp : Schema_Op<"struct", [Pure]> {
  let summary = "Aggregate per-property verdicts into an object schema verdict.";
  let description = [{
    Models a JSON Schema `object` with a fixed property set. `$field_names`
    declares the properties in declaration order and must be exactly parallel
    to the `$field_results` operands, each of which is the verdict produced by
    the validator subtree for that property.

    `$required_fields` must be a subset of `$field_names`; a missing property
    that is not required is vacuously valid. The `additional_properties` unit
    attribute permits properties outside the declared set.

    Example:
    ```mlir
    %ok = schema.struct %doc as "Person"
            fields ["name", "age"] required ["name"]
            validators(%name_ok, %age_ok)
          : (!schema.value, i1, i1) -> i1
    ```
  }];

  let arguments = (ins
    Schema_ValueType:$input,
    Variadic<Schema_Verdict>:$field_results,
    StrAttr:$type_name,
    StrArrayAttr:$field_names,
    OptionalAttr<StrArrayAttr>:$required_fields,
    UnitAttr:$additional_properties
  );

  let results = (outs Schema_Verdict:$valid);

  let assemblyFormat = [{
    $input `as` $type_name
    `fields` $field_names
    (`required` $required_fields^)?
    `validators` `(` $field_results `)`
    attr-dict `:` functional-type(operands, results)
  }];

  let builders = [
    OpBuilder<(ins "::mlir::Value":$input,
                   "::llvm::StringRef":$typeName,
                   "::llvm::ArrayRef<::llvm::StringRef>":$fieldNames,
                   "::mlir::ValueRange":$fieldResults), [{
      build($_builder, $_state, $_builder.getI1Type(), input, fieldResults,
            $_builder.getStringAttr(typeName),
            $_builder.getStrArrayAttr(fieldNames),
            /*required_fields=*/nullptr,
            /*additional_properties=*/nullptr);
    }]>
  ];

  let hasVerifier = 1;
}

#endif // SCHEMA_OPS_TD
