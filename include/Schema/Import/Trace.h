//===- Trace.h - Front-end trace for external viewers -----------*- C++ -*-===//
//
// Writes the JSON document produced by `--emit-trace=<file>`: the tokens, the
// AST, the diagnostics and the IR of each pipeline stage, all tied back to
// source ranges. The format is specified in `docs/trace-format.md`.
//
//===----------------------------------------------------------------------===//

#ifndef SCHEMA_IMPORT_TRACE_H
#define SCHEMA_IMPORT_TRACE_H

#include "Schema/Import/AST.h"
#include "Schema/Import/Lexer.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace mlir::schema {

/// Bumped on any incompatible change to the trace format.
inline constexpr int kTraceFormatVersion = 1;

/// A diagnostic captured while the front end ran.
struct TraceDiagnostic {
  std::string severity;
  std::string message;
  Location location;
  std::vector<TraceDiagnostic> notes;

  static TraceDiagnostic from(Diagnostic &diag);
};

/// The IR after one pipeline stage.
struct TraceStage {
  std::string name;
  ModuleOp module;
};

/// Writes the trace. `root` may be null (parse failure) and `stages` empty
/// (import failure); the trace then holds whatever was produced.
void writeTrace(llvm::raw_ostream &os, const json::SourceFile &file,
                ArrayRef<json::Token> tokens, const json::SchemaNode *root,
                ArrayRef<TraceDiagnostic> diagnostics,
                ArrayRef<TraceStage> stages);

} // namespace mlir::schema

#endif // SCHEMA_IMPORT_TRACE_H
