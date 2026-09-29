add_mlir_library(MLIRSchemaImport
  Lexer.cpp
  Translation.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Schema/Import

  LINK_LIBS PUBLIC
  MLIRSchema
  MLIRIR
  MLIRSupport
  MLIRTranslateLib
)
