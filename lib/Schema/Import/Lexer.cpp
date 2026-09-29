//===- Lexer.cpp - JSON lexer for the schema front end --------------------===//

#include "Schema/Import/Lexer.h"

#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/MLIRContext.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <algorithm>

using namespace mlir;
using namespace mlir::schema::json;

//===----------------------------------------------------------------------===//
// SourceFile
//===----------------------------------------------------------------------===//

SourceFile::SourceFile(MLIRContext *context, StringRef name, StringRef text)
    : context(context), name(name), text(text) {
  lineStarts.push_back(0);
  for (unsigned i = 0, e = text.size(); i < e; ++i)
    if (text[i] == '\n')
      lineStarts.push_back(i + 1);
}

Location SourceFile::getLoc(SourcePos pos) const {
  return FileLineColLoc::get(context, name, pos.line, pos.column);
}

InFlightDiagnostic SourceFile::emitError(SourcePos pos,
                                         const Twine &message) const {
  return mlir::emitError(getLoc(pos), message);
}

InFlightDiagnostic SourceFile::emitWarning(SourcePos pos,
                                           const Twine &message) const {
  return mlir::emitWarning(getLoc(pos), message);
}

unsigned SourceFile::getOffset(unsigned line, unsigned column) const {
  if (line == 0 || lineStarts.empty())
    return 0;
  unsigned lineIndex = std::min<unsigned>(line, lineStarts.size()) - 1;
  unsigned offset = lineStarts[lineIndex] + (column ? column - 1 : 0);
  return std::min<unsigned>(offset, text.size());
}

//===----------------------------------------------------------------------===//
// Token kinds
//===----------------------------------------------------------------------===//

StringRef mlir::schema::json::stringifyTokenKind(TokenKind kind) {
  switch (kind) {
  case TokenKind::LBrace:
    return "l_brace";
  case TokenKind::RBrace:
    return "r_brace";
  case TokenKind::LSquare:
    return "l_square";
  case TokenKind::RSquare:
    return "r_square";
  case TokenKind::Colon:
    return "colon";
  case TokenKind::Comma:
    return "comma";
  case TokenKind::String:
    return "string";
  case TokenKind::Number:
    return "number";
  case TokenKind::True:
    return "true";
  case TokenKind::False:
    return "false";
  case TokenKind::Null:
    return "null";
  case TokenKind::Eof:
    return "eof";
  case TokenKind::Error:
    return "error";
  }
  llvm_unreachable("unknown token kind");
}

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

Lexer::Lexer(const SourceFile &file, bool allowComments)
    : file(file), text(file.getText()), allowComments(allowComments) {
  // A UTF-8 byte order mark is not part of the JSON text (RFC 8259 §8.1).
  if (text.starts_with("\xEF\xBB\xBF")) {
    pos.offset = 3;
    pos.column = 4;
  }
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;
  do {
    tokens.push_back(lexToken());
  } while (!tokens.back().is(TokenKind::Eof));
  return tokens;
}

char Lexer::peek(unsigned ahead) const {
  unsigned index = pos.offset + ahead;
  return index < text.size() ? text[index] : '\0';
}

void Lexer::advance() {
  if (pos.offset >= text.size())
    return;
  if (text[pos.offset] == '\n') {
    ++pos.line;
    pos.column = 1;
  } else {
    ++pos.column;
  }
  ++pos.offset;
}

void Lexer::skipWhitespace() {
  while (pos.offset < text.size()) {
    char c = text[pos.offset];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      return;
    advance();
  }
}

Token Lexer::makeToken(TokenKind kind, SourcePos begin) {
  return Token{
      kind, {begin, pos}, text.slice(begin.offset, pos.offset), std::string()};
}

Token Lexer::makeError(SourcePos begin, SourcePos at, const Twine &message) {
  failed = true;
  file.emitError(at, message);
  return makeToken(TokenKind::Error, begin);
}

Token Lexer::lexToken() {
  skipWhitespace();
  while (allowComments && peek() == '/' && (peek(1) == '/' || peek(1) == '*')) {
    if (std::optional<Token> error = skipComment())
      return *error;
    skipWhitespace();
  }
  SourcePos begin = here();
  if (pos.offset >= text.size())
    return makeToken(TokenKind::Eof, begin);

  char c = peek();
  switch (c) {
  case '{':
    advance();
    return makeToken(TokenKind::LBrace, begin);
  case '}':
    advance();
    return makeToken(TokenKind::RBrace, begin);
  case '[':
    advance();
    return makeToken(TokenKind::LSquare, begin);
  case ']':
    advance();
    return makeToken(TokenKind::RSquare, begin);
  case ':':
    advance();
    return makeToken(TokenKind::Colon, begin);
  case ',':
    advance();
    return makeToken(TokenKind::Comma, begin);
  case '"':
    return lexString();
  case '/':
    if (peek(1) == '/' || peek(1) == '*') {
      if (std::optional<Token> error = skipComment())
        return *error;
      return makeError(begin, begin,
                       "comments are not allowed in JSON (see "
                       "--allow-comments)");
    }
    advance();
    return makeError(begin, begin, "unexpected character '/'");
  default:
    break;
  }

  if (c == '-' || llvm::isDigit(c))
    return lexNumber();
  if (llvm::isAlpha(c) || c == '_')
    return lexWord();

  advance();
  // Keep a multi-byte UTF-8 sequence in one error token.
  while (pos.offset < text.size() && (peek() & 0xC0) == 0x80)
    advance();
  if (llvm::isPrint(c))
    return makeError(begin, begin, "unexpected character '" + Twine(c) + "'");
  return makeError(begin, begin, "unexpected byte in JSON text");
}

std::optional<Token> Lexer::skipComment() {
  SourcePos begin = here();
  advance();
  if (peek() == '/') {
    while (pos.offset < text.size() && peek() != '\n')
      advance();
    return std::nullopt;
  }
  advance(); // '*'
  while (pos.offset < text.size()) {
    if (peek() == '*' && peek(1) == '/') {
      advance();
      advance();
      return std::nullopt;
    }
    advance();
  }
  return makeError(begin, begin, "unterminated block comment");
}

/// Appends `codePoint` to `out` as UTF-8.
static void appendUTF8(uint32_t codePoint, std::string &out) {
  char buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
  char *end = buffer;
  if (llvm::ConvertCodePointToUTF8(codePoint, end))
    out.append(buffer, end);
}

Token Lexer::lexString() {
  SourcePos begin = here();
  advance(); // opening quote
  std::string value;
  bool bad = false;

  // Reports the first problem in the string, then keeps scanning so the
  // token still ends at the closing quote.
  auto fail = [&](SourcePos at, const Twine &message) {
    if (!bad)
      file.emitError(at, message);
    bad = true;
  };

  // Reads four hex digits after `\u`. Returns false if they are missing.
  auto readHex4 = [&](uint32_t &result) {
    result = 0;
    for (int i = 0; i < 4; ++i) {
      char h = peek();
      if (!llvm::isHexDigit(h))
        return false;
      result = result * 16 + llvm::hexDigitValue(h);
      advance();
    }
    return true;
  };

  while (true) {
    SourcePos at = here();
    if (pos.offset >= text.size() || peek() == '\n') {
      fail(begin, "unterminated string");
      break;
    }
    char c = peek();
    if (c == '"') {
      advance();
      break;
    }
    if (static_cast<unsigned char>(c) < 0x20) {
      fail(at, "control character in string must be escaped");
      advance();
      continue;
    }
    if (c != '\\') {
      value.push_back(c);
      advance();
      continue;
    }

    advance(); // backslash
    char escape = peek();
    switch (escape) {
    case '"':
    case '\\':
    case '/':
      value.push_back(escape);
      advance();
      continue;
    case 'b':
      value.push_back('\b');
      advance();
      continue;
    case 'f':
      value.push_back('\f');
      advance();
      continue;
    case 'n':
      value.push_back('\n');
      advance();
      continue;
    case 'r':
      value.push_back('\r');
      advance();
      continue;
    case 't':
      value.push_back('\t');
      advance();
      continue;
    case 'u': {
      advance();
      uint32_t unit;
      if (!readHex4(unit)) {
        fail(at, "expected four hex digits after '\\u'");
        continue;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        fail(at, "unpaired low surrogate in '\\u' escape");
        continue;
      }
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        uint32_t low;
        if (peek() != '\\' || peek(1) != 'u') {
          fail(at, "unpaired high surrogate in '\\u' escape");
          continue;
        }
        advance();
        advance();
        if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) {
          fail(at, "unpaired high surrogate in '\\u' escape");
          continue;
        }
        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
      }
      appendUTF8(unit, value);
      continue;
    }
    default:
      fail(at, "invalid escape sequence in string");
      if (escape != '\n' && pos.offset < text.size())
        advance();
      continue;
    }
  }

  if (bad) {
    failed = true;
    return makeToken(TokenKind::Error, begin);
  }
  Token token = makeToken(TokenKind::String, begin);
  token.value = std::move(value);
  return token;
}

Token Lexer::lexNumber() {
  // number = [ "-" ] int [ frac ] [ exp ]      (RFC 8259 §6)
  SourcePos begin = here();

  // On error, swallow the rest of the number-like run so that `01.5e` is one
  // error token rather than several.
  auto fail = [&](const Twine &message) {
    SourcePos at = here();
    while (llvm::isAlnum(peek()) || peek() == '.' || peek() == '+' ||
           peek() == '-')
      advance();
    return makeError(begin, at, message);
  };

  if (peek() == '-')
    advance();

  if (peek() == '0') {
    advance();
    if (llvm::isDigit(peek()))
      return fail("leading zeros are not allowed in JSON numbers");
  } else if (llvm::isDigit(peek())) {
    while (llvm::isDigit(peek()))
      advance();
  } else {
    return fail("expected a digit after '-'");
  }

  if (peek() == '.') {
    advance();
    if (!llvm::isDigit(peek()))
      return fail("expected a digit after '.'");
    while (llvm::isDigit(peek()))
      advance();
  }

  if (peek() == 'e' || peek() == 'E') {
    advance();
    if (peek() == '+' || peek() == '-')
      advance();
    if (!llvm::isDigit(peek()))
      return fail("expected a digit in the exponent");
    while (llvm::isDigit(peek()))
      advance();
  }

  if (llvm::isAlpha(peek()) || peek() == '.')
    return fail("invalid character in number");
  return makeToken(TokenKind::Number, begin);
}

Token Lexer::lexWord() {
  SourcePos begin = here();
  while (llvm::isAlnum(peek()) || peek() == '_')
    advance();
  StringRef word = text.slice(begin.offset, pos.offset);
  if (word == "true")
    return makeToken(TokenKind::True, begin);
  if (word == "false")
    return makeToken(TokenKind::False, begin);
  if (word == "null")
    return makeToken(TokenKind::Null, begin);
  return makeError(begin, begin,
                   "invalid literal '" + word +
                       "'; expected 'true', 'false' or 'null'");
}
