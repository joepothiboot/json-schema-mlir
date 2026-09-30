add_mlir_library(MLIRSchemaImport
  AST.cpp
  Importer.cpp
  Lexer.cpp
  Parser.cpp
  Translation.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/Schema/Import

  LINK_LIBS PUBLIC
  MLIRSchema
  MLIRArithDialect
  MLIRFuncDialect
  MLIRIR
  MLIRSupport
  MLIRTranslateLib
)
