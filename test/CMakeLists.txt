configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

set(SCHEMA_TEST_DEPENDS FileCheck count not schema-opt)
if(TARGET mlir-translate)
  list(APPEND SCHEMA_TEST_DEPENDS mlir-translate)
endif()

add_lit_testsuite(check-schema "Running the json-schema-mlir regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${SCHEMA_TEST_DEPENDS}
)
set_target_properties(check-schema PROPERTIES FOLDER "Tests")
add_lit_testsuites(SCHEMA ${CMAKE_CURRENT_SOURCE_DIR} DEPENDS ${SCHEMA_TEST_DEPENDS})