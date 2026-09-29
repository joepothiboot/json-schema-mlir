//===- Parser.h - Recursive-descent JSON Schema parser ----------*- C++ -*-===//
#ifndef SCHEMA_IMPORT_PARSER_H
#define SCHEMA_IMPORT_PARSER_H

#include "Schema/Import/AST.h"
#include "Schema/Import/Lexer.h"

#include "llvm/ADT/ArrayRef.h"

#include <memory>

namespace mlir::schema::json {

struct ParsedSchema {
  /// The root schema. May be partial (or null) when `failed` is set.
  std::unique_ptr<SchemaNode> root;
  /// Ids are assigned in pre-order from 0; this is one past the largest.
  unsigned numNodes = 0;
  bool failed = false;
};

/// Parses `tokens` (as produced by `Lexer::tokenize`) into a schema AST.
/// Syntax errors are reported as located diagnostics; the parser recovers at
/// the next `,`, `}` or `]` so that one run reports several errors.
ParsedSchema parseSchema(const SourceFile &file, ArrayRef<Token> tokens);

} // namespace mlir::schema::json

#endif // SCHEMA_IMPORT_PARSER_H
