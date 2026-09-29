//===- Translation.cpp - `--import-json-schema` ---------------------------===//
//
// Drives the front end: lex -> parse -> import. `--dump-tokens` and
// `--dump-ast` stop after the corresponding stage and print it instead of IR.
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Translation.h"

#include "Schema/Import/AST.h"
#include "Schema/Import/Lexer.h"
#include "Schema/Import/Parser.h"

#include "mlir/IR/MLIRContext.h"
#include "mlir/Tools/mlir-translate/Translation.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
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

static LogicalResult importJsonSchema(llvm::SourceMgr &sourceMgr,
                                      raw_ostream &output,
                                      MLIRContext *context) {
  const llvm::MemoryBuffer *buffer =
      sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());
  json::SourceFile file(context, buffer->getBufferIdentifier(),
                        buffer->getBuffer());

  json::Lexer lexer(file, allowComments);
  std::vector<json::Token> tokens = lexer.tokenize();
  if (dumpTokens) {
    printTokens(tokens, output);
    return failure(lexer.hadError());
  }

  json::ParsedSchema parsed = json::parseSchema(file, tokens);
  if (dumpAst) {
    if (parsed.root)
      json::dumpAst(*parsed.root, output);
    return failure(lexer.hadError() || parsed.failed);
  }

  return emitError(UnknownLoc::get(context),
                   "only --dump-tokens and --dump-ast are implemented so far");
}

void mlir::schema::registerImportJsonSchemaTranslation() {
  TranslateRegistration registration(
      "import-json-schema", "Import a JSON Schema document into `schema` IR",
      [](const std::shared_ptr<llvm::SourceMgr> &sourceMgr, raw_ostream &output,
         MLIRContext *context) {
        return importJsonSchema(*sourceMgr, output, context);
      });
}
