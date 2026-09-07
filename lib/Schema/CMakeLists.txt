add_mlir_dialect_library(MLIRSchema
  SchemaDialect.cpp
  SchemaOps.cpp
  SchemaCanonicalizerPass.cpp
  LowerToStandard.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Schema

  DEPENDS
  MLIRSchemaOpsIncGen

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRPass
  MLIRRewrite
  MLIRSupport
  MLIRTransforms
  MLIRTransformUtils
  MLIRArithDialect
  MLIRFuncDialect
  MLIRMathDialect
  MLIRSCFDialect
  MLIRSCFTransforms
  MLIRSideEffectInterfaces
)