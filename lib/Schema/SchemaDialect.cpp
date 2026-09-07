#include "Schema/SchemaDialect.h"

#include "Schema/SchemaOps.h"
#include "Schema/SchemaTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::schema;

// The type definitions must precede the dialect definitions: the generated
// SchemaDialect::parseType/printType call the file-local generatedTypeParser
// and generatedTypePrinter emitted by GET_TYPEDEF_CLASSES.
#define GET_TYPEDEF_CLASSES
#include "Schema/SchemaOpsTypes.cpp.inc"

#include "Schema/SchemaOpsDialect.cpp.inc"

void SchemaDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "Schema/SchemaOps.cpp.inc"
      >();
  registerTypes();
}

void SchemaDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "Schema/SchemaOpsTypes.cpp.inc"
      >();
}