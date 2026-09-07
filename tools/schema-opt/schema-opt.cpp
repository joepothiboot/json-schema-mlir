// Minimal `mlir-opt`-style driver for the out-of-tree `schema` dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/Conversion/Passes.h"
#include "Schema/SchemaDialect.h"
#include "Schema/SchemaOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "Schema/SchemaPasses.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "mlir/Transforms/Passes.h"

int main(int argc, char **argv) {
  mlir::registerAllPasses();

  // Registers --schema-canonicalize, --lower-schema-to-std, and the complete
  // standard/LLVM lowering pipelines.
  mlir::schema::registerSchemaPasses();

  mlir::DialectRegistry registry;

  // Upstream dialects, so `schema` IR can be embedded in func/scf/arith and
  // later lowered; plus the extensions that attach external model interfaces.
  mlir::registerAllDialects(registry);
  mlir::registerAllExtensions(registry);

  // Our out-of-tree dialect.
  registry.insert<mlir::schema::SchemaDialect>();

  return mlir::asMainReturnCode(mlir::MlirOptMain(
      argc, argv, "json-schema-mlir optimizer driver\n", registry));
}