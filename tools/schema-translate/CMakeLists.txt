set(LIBS
  MLIRSchemaImport
  MLIRSchema
  MLIRTranslateLib
  MLIRIR
  MLIRSupport
)

add_llvm_executable(schema-translate schema-translate.cpp)
llvm_update_compile_flags(schema-translate)
target_link_libraries(schema-translate PRIVATE ${LIBS})
mlir_check_all_link_libraries(schema-translate)
