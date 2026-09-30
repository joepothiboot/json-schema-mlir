//===- Translation.cpp - `--import-json-schema` ---------------------------===//
//
// Drives the front end: lex -> parse -> import. `--dump-tokens` and
// `--dump-ast` stop after the corresponding stage and print it instead of IR.
// `--emit-trace=<file>` additionally records every stage (plus the IR after
// `--schema-canonicalize`) as JSON, including when a stage fails.
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Translation.h"

#include "Schema/Import/AST.h"
#include "Schema/Import/Importer.h"
#include "Schema/Import/Lexer.h"
#include "Schema/Import/Parser.h"
#include "Schema/Import/Trace.h"
#include "Schema/SchemaPasses.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Tools/mlir-translate/Translation.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::schema;

static llvm::cl::OptionCategory importCategory("JSON Schema import options");

static llvm::cl::opt<bool>
    dumpTokens("dump-tokens",
               llvm::cl::desc("Print the JSON token stream and stop"),
               llvm::cl::cat(importCategory));

static llvm::cl::opt<bool> allowComments(
    "allow-comments",
    llvm::cl::desc("Accept // and /* */ comments in the JSON input (JSONC)"),
    llvm::cl::cat(importCategory));

static llvm::cl::opt<bool>
    dumpAst("dump-ast", llvm::cl::desc("Print the schema AST and stop"),
            llvm::cl::cat(importCategory));

static llvm::cl::opt<std::string> emitTrace(
    "emit-trace",
    llvm::cl::desc("Write tokens, AST, diagnostics and IR (as imported and "
                   "after --schema-canonicalize) to this JSON file"),
    llvm::cl::value_desc("file.json"), llvm::cl::cat(importCategory));

static void printTokens(ArrayRef<json::Token> tokens, raw_ostream &os) {
  for (const json::Token &token : tokens) {
    const json::SourceRange &range = token.range;
    os << range.begin.line << ':' << range.begin.column << ' '
       << json::stringifyTokenKind(token.kind) << " [" << range.begin.offset
       << ',' << range.end.offset << ')';
    if (!token.spelling.empty())
      os << ' ' << token.spelling;
    os << '\n';
  }
}

namespace {
/// What the front end produced so far, for `--emit-trace`.
struct TraceState {
  std::vector<json::Token> tokens;
  json::ParsedSchema parsed;
  std::vector<TraceDiagnostic> diagnostics;
  OwningOpRef<ModuleOp> imported;
  OwningOpRef<ModuleOp> canonicalized;
};
} // namespace

static LogicalResult runFrontEnd(const json::SourceFile &file,
                                 raw_ostream &output, TraceState &state) {
  json::Lexer lexer(file, allowComments);
  state.tokens = lexer.tokenize();
  if (dumpTokens) {
    printTokens(state.tokens, output);
    return failure(lexer.hadError());
  }

  state.parsed = json::parseSchema(file, state.tokens);
  if (dumpAst) {
    if (state.parsed.root)
      json::dumpAst(*state.parsed.root, output);
    return failure(lexer.hadError() || state.parsed.failed);
  }
  if (lexer.hadError() || state.parsed.failed || !state.parsed.root)
    return failure();

  state.imported = importSchema(*state.parsed.root, file);
  if (!state.imported)
    return failure();
  state.imported->print(output);

  if (!emitTrace.empty()) {
    // Canonicalize a copy; the tool's output stays the imported IR.
    MLIRContext *context = file.getContext();
    state.canonicalized = state.imported->clone();
    PassManager pm(context, ModuleOp::getOperationName());
    pm.addNestedPass<func::FuncOp>(createSchemaCanonicalizerPass());
    if (failed(pm.run(*state.canonicalized))) {
      state.canonicalized = nullptr;
      return failure();
    }
  }
  return success();
}

static LogicalResult importJsonSchema(llvm::SourceMgr &sourceMgr,
                                      raw_ostream &output,
                                      MLIRContext *context) {
  const llvm::MemoryBuffer *buffer =
      sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());
  json::SourceFile file(context, buffer->getBufferIdentifier(),
                        buffer->getBuffer());

  if (emitTrace.empty()) {
    TraceState state;
    return runFrontEnd(file, output, state);
  }

  // Record diagnostics for the trace and pass them on to the tool's handler
  // (printing or -verify-diagnostics).
  TraceState state;
  LogicalResult result = success();
  {
    ScopedDiagnosticHandler recorder(context, [&](Diagnostic &diag) {
      state.diagnostics.push_back(TraceDiagnostic::from(diag));
      return failure();
    });
    result = runFrontEnd(file, output, state);
  }

  std::string message;
  std::unique_ptr<llvm::ToolOutputFile> traceFile =
      openOutputFile(emitTrace, &message);
  if (!traceFile)
    return emitError(UnknownLoc::get(context))
           << "cannot write trace: " << message;
  SmallVector<TraceStage> stages;
  if (state.imported)
    stages.push_back({"import", *state.imported});
  if (state.canonicalized)
    stages.push_back({"schema-canonicalize", *state.canonicalized});
  writeTrace(traceFile->os(), file, state.tokens, state.parsed.root.get(),
             state.diagnostics, stages);
  traceFile->keep();
  return result;
}

void mlir::schema::registerImportJsonSchemaTranslation() {
  TranslateRegistration registration(
      "import-json-schema", "Import a JSON Schema document into `schema` IR",
      [](const std::shared_ptr<llvm::SourceMgr> &sourceMgr, raw_ostream &output,
         MLIRContext *context) {
        return importJsonSchema(*sourceMgr, output, context);
      });
}
