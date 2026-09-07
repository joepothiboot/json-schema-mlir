# Generates, from SchemaOps.td:
#   SchemaOps.h.inc / SchemaOps.cpp.inc               (-gen-op-decls/-defs)
#   SchemaOpsTypes.h.inc / SchemaOpsTypes.cpp.inc     (-gen-typedef-decls/-defs)
#   SchemaOpsDialect.h.inc / SchemaOpsDialect.cpp.inc (-gen-dialect-decls/-defs)
# and exposes the target MLIRSchemaOpsIncGen.
add_mlir_dialect(SchemaOps schema)

# Standalone documentation targets.
add_mlir_doc(SchemaDialect SchemaDialect Schema/ -gen-dialect-doc)
add_mlir_doc(SchemaOps     SchemaOps     Schema/ -gen-op-doc)

# Explicit mlir_tablegen invocation for the bytecode/enum-style extras and to
# show how additional generators are attached to the same .td source.
set(LLVM_TARGET_DEFINITIONS SchemaOps.td)
mlir_tablegen(SchemaOpsAttrDefs.h.inc   -gen-attrdef-decls -attrdefs-dialect=schema)
mlir_tablegen(SchemaOpsAttrDefs.cpp.inc -gen-attrdef-defs  -attrdefs-dialect=schema)
add_public_tablegen_target(MLIRSchemaAttrDefsIncGen)
