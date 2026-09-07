cmake_minimum_required(VERSION 3.20.0)
project(json-schema-mlir LANGUAGES CXX C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- C++20 -------------------------------------------------------------------
# Must be declared before HandleLLVMOptions, which materializes the -std flag.
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Locate the LLVM / MLIR installation -------------------------------------
find_package(LLVM REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

message(STATUS "Using LLVMConfig.cmake in : ${LLVM_DIR}")
message(STATUS "Using MLIRConfig.cmake in : ${MLIR_DIR}")
message(STATUS "LLVM/MLIR version         : ${LLVM_PACKAGE_VERSION}")

# Mirror the in-tree layout so AddMLIR helpers behave identically out-of-tree.
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR            ${CMAKE_BINARY_DIR})

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)   # may reset CMAKE_CXX_STANDARD -> re-assert below

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

# --- Include / link search paths ---------------------------------------------
# TableGen.cmake forwards INCLUDE_DIRECTORIES as -I flags to mlir-tblgen,
# so `include "Schema/SchemaDialect.td"` resolves from these entries.
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_BINARY_DIR}/include)
link_directories(${LLVM_BUILD_LIBRARY_DIR})
add_definitions(${LLVM_DEFINITIONS})

# add_mlir_dialect()/add_mlir_doc() attach to these aggregate targets, which
# only exist in-tree. Create no-op stand-ins when building standalone.
if(NOT TARGET mlir-headers)
  add_custom_target(mlir-headers)
endif()
if(NOT TARGET mlir-generic-headers)
  add_custom_target(mlir-generic-headers)
endif()
if(NOT TARGET mlir-doc)
  add_custom_target(mlir-doc)
endif()

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(test)