//===- schema-opt.cpp - JSON Schema dialect optimizer driver --------------===//
//
// Minimal `mlir-opt`-style driver for the out-of-tree `schema` dialect.
//
//===----------------------------------------------------------------------===//

#include "Schema/SchemaDialect.h"
#include "Schema/SchemaOps.h"

#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

int main(int argc, char **argv) {
  // Upstream passes (canonicalize, cse, print-op-stats, ...).
  mlir::registerAllPasses();

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