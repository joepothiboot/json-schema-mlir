//===- Lexer.h - JSON lexer for the schema front end ------------*- C++ -*-===//
//
// A hand-written RFC 8259 lexer. Every token carries its byte range and the
// 1-based line:column of both ends, so later stages (and the trace) can point
// back into the source without a SourceMgr lookup.
//
//===----------------------------------------------------------------------===//

#ifndef SCHEMA_IMPORT_LEXER_H
#define SCHEMA_IMPORT_LEXER_H

#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <utility>
#include <vector>

namespace mlir {
class MLIRContext;
} // namespace mlir

namespace mlir::schema::json {

/// A position in the source buffer. `line` and `column` are 1-based; the
/// column counts bytes, matching `FileLineColLoc` and `llvm::SourceMgr`.
struct SourcePos {
  unsigned offset = 0;
  unsigned line = 1;
  unsigned column = 1;
};

/// A half-open byte range `[begin, end)`.
struct SourceRange {
  SourcePos begin;
  SourcePos end;
};

/// The source buffer plus the context needed to build locations and emit
/// diagnostics that point into it.
class SourceFile {
public:
  SourceFile(MLIRContext *context, StringRef name, StringRef text);

  MLIRContext *getContext() const { return context; }
  StringRef getName() const { return name; }
  StringRef getText() const { return text; }

  /// A `FileLineColLoc` for `pos`.
  Location getLoc(SourcePos pos) const;

  InFlightDiagnostic emitError(SourcePos pos, const Twine &message) const;
  InFlightDiagnostic emitWarning(SourcePos pos, const Twine &message) const;

  /// Byte offset of `line:column`, clamped to the buffer.
  unsigned getOffset(unsigned line, unsigned column) const;

private:
  MLIRContext *context;
  StringRef name;
  StringRef text;
  std::vector<unsigned> lineStarts;
};

enum class TokenKind {
  LBrace,
  RBrace,
  LSquare,
  RSquare,
  Colon,
  Comma,
  String,
  Number,
  True,
  False,
  Null,
  Eof,
  Error,
};

/// Spelling used by `--dump-tokens` and the trace (`l_brace`, `string`, ...).
StringRef stringifyTokenKind(TokenKind kind);

struct Token {
  TokenKind kind;
  SourceRange range;
  /// The raw bytes of the token, quotes and escapes included.
  StringRef spelling;
  /// The decoded value of a `String` token; empty otherwise.
  std::string value;

  bool is(TokenKind k) const { return kind == k; }
};

/// Splits a buffer into tokens. Lexical errors are reported through the
/// context's diagnostic engine and produce `Error` tokens, so lexing always
/// runs to the end of the buffer.
class Lexer {
public:
  explicit Lexer(const SourceFile &file);

  /// Lexes the whole buffer. The last token is always `Eof`.
  std::vector<Token> tokenize();

  bool hadError() const { return failed; }

private:
  Token lexToken();
  Token lexString();
  Token lexNumber();
  Token lexWord();

  Token makeToken(TokenKind kind, SourcePos begin);
  Token makeError(SourcePos begin, SourcePos at, const Twine &message);

  char peek(unsigned ahead = 0) const;
  void advance();
  void skipWhitespace();
  SourcePos here() const { return pos; }

  const SourceFile &file;
  StringRef text;
  SourcePos pos;
  bool failed = false;
};

} // namespace mlir::schema::json

#endif // SCHEMA_IMPORT_LEXER_H
