#ifndef SCHEMA_SCHEMATYPES_H
#define SCHEMA_SCHEMATYPES_H

#include "Schema/SchemaDialect.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"

#define GET_TYPEDEF_CLASSES
#include "Schema/SchemaOpsTypes.h.inc"

#endif // SCHEMA_SCHEMATYPES_H