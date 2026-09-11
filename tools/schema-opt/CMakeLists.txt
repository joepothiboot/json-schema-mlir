get_property(dialect_libs    GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
get_property(extension_libs  GLOBAL PROPERTY MLIR_EXTENSION_LIBS)

set(LIBS
  ${dialect_libs}
  ${conversion_libs}
  ${extension_libs}
  MLIRSchema
  MLIRRegisterAllPasses
  MLIRRegisterAllDialects
  MLIRRegisterAllExtensions
  MLIROptLib
  MLIRParser
  MLIRSupport
  MLIRIR
)

add_llvm_executable(schema-opt schema-opt.cpp)
llvm_update_compile_flags(schema-opt)
target_link_libraries(schema-opt PRIVATE ${LIBS})
mlir_check_all_link_libraries(schema-opt)