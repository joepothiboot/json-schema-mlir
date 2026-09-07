#ifndef SCHEMA_SCHEMAOPS_H
#define SCHEMA_SCHEMAOPS_H

#include "Schema/SchemaDialect.h"
#include "Schema/SchemaTypes.h"
#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#define GET_OP_CLASSES
#include "Schema/SchemaOps.h.inc"

#endif // SCHEMA_SCHEMAOPS_H