// `mlir-translate`-style driver for the JSON Schema front end.
//
// Importing a foreign format is a translation in MLIR terms (compare
// `mlir-translate --import-llvm`), so it lives here rather than in
// `schema-opt`, whose input is always MLIR.
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Translation.h"

#include "mlir/Support/LogicalResult.h"
#include "mlir/Tools/mlir-translate/MlirTranslateMain.h"

int main(int argc, char **argv) {
  mlir::schema::registerImportJsonSchemaTranslation();
  return mlir::failed(mlir::mlirTranslateMain(
      argc, argv, "json-schema-mlir translation driver\n"));
}
