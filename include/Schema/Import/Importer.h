//===- Importer.h - Schema AST to `schema` dialect --------------*- C++ -*-===//
#ifndef SCHEMA_IMPORT_IMPORTER_H
#define SCHEMA_IMPORT_IMPORTER_H

#include "Schema/Import/AST.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/OwningOpRef.h"

namespace mlir::schema {

/// Builds a module holding one `func.func @validate_<name>` for `root`.
///
/// The function takes the document as `%arg0`, followed by one
/// `!schema.value` per distinct property path, in the order the properties
/// are first reached (depth-first, declaration order). It returns the `i1`
/// verdict.
///
/// Every op's location is the `FileLineColLoc` of the AST node that produced
/// it: the keyword for a validator, `properties` for a `schema.struct`, the
/// schema object for the `arith.andi` joining its conjuncts. Returns null
/// after reporting diagnostics when the schema is invalid or uses something
/// the dialect cannot express.
OwningOpRef<ModuleOp> importSchema(const json::SchemaNode &root,
                                   const json::SourceFile &file);

} // namespace mlir::schema

#endif // SCHEMA_IMPORT_IMPORTER_H
