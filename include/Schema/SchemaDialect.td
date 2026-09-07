//===- SchemaDialect.td - JSON Schema dialect definition ---*- tablegen -*-===//
#ifndef SCHEMA_DIALECT_TD
#define SCHEMA_DIALECT_TD

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/OpBase.td"

//===----------------------------------------------------------------------===//
// Dialect
//===----------------------------------------------------------------------===//

def Schema_Dialect : Dialect {
  let name = "schema";
  let summary = "A dialect for expressing JSON Schema validation rules as IR.";
  let description = [{
    The `schema` dialect models JSON Schema (Draft 2020-12) constraints as
    side-effect-free MLIR operations. Each validation operation consumes an
    opaque `!schema.value` handle and yields an `i1` verdict, allowing the
    canonicalizer and CSE to fuse redundant constraints before the IR is
    lowered to `scf`/`cf` + `func` calls into a runtime library.
  }];

  let cppNamespace = "::mlir::schema";

  // Emit the dialect-level parseType/printType hooks that dispatch to the
  // TableGen-generated per-type parsers.
  let useDefaultTypePrinterParser = 1;

  let extraClassDeclaration = [{
    /// Registers all `schema` dialect types. Defined in SchemaDialect.cpp.
    void registerTypes();
  }];
}

//===----------------------------------------------------------------------===//
// Base classes
//===----------------------------------------------------------------------===//

class Schema_Op<string mnemonic, list<Trait> traits = []>
    : Op<Schema_Dialect, mnemonic, traits>;

class Schema_Type<string name, string typeMnemonic, list<Trait> traits = []>
    : TypeDef<Schema_Dialect, name, traits> {
  let mnemonic = typeMnemonic;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

def Schema_ValueType : Schema_Type<"Value", "value"> {
  let summary = "An opaque handle to a runtime JSON value.";
  let description = [{
    `!schema.value` is an opaque, unranked handle to a decoded JSON document
    node (object, array, string, number, boolean or null). It carries no
    parameters: the dialect deliberately keeps the dynamic type unmodelled so
    that validation ops remain the single source of truth for constraints.

    Example:
    ```mlir
    func.func @check(%doc: !schema.value) -> i1
    ```
  }];
}

#endif // SCHEMA_DIALECT_TD