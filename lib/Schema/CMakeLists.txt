add_mlir_dialect_library(MLIRSchema
  SchemaDialect.cpp
  SchemaOps.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Schema

  DEPENDS
  MLIRSchemaOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRSupport
  MLIRSideEffectInterfaces
)