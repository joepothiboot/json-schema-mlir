//===- Parser.cpp - Recursive-descent JSON Schema parser ------------------===//
//
// Grammar (RFC 8259, with schema objects recognised by position):
//
//   document := schema EOF
//   schema   := '{' (member (',' member)*)? '}'
//   member   := STRING ':' keyword-value
//   value    := object | array | STRING | NUMBER | 'true' | 'false' | 'null'
//
// The parser checks the *shape* of each keyword's value (e.g. `minLength` is
// a non-negative integer). Whether the dialect can express a keyword is the
// importer's decision, so unknown keywords are kept as `UnknownKeyword`.
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Parser.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"

#include <cmath>

using namespace mlir;
using namespace mlir::schema::json;

namespace {

/// Deeper nesting is rejected so that hostile input cannot exhaust the stack.
constexpr unsigned kMaxDepth = 128;

/// The largest integer a double represents exactly.
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53

class Parser {
public:
  Parser(const SourceFile &file, ArrayRef<Token> tokens)
      : file(file), tokens(tokens) {
    assert(!tokens.empty() && tokens.back().is(TokenKind::Eof) &&
           "token stream must end with EOF");
  }

  ParsedSchema parseDocument();

private:
  using MemberFn = llvm::function_ref<void(const Token &key)>;
  using ElementFn = llvm::function_ref<void()>;

  //===--------------------------------------------------------------------===//
  // Token stream
  //===--------------------------------------------------------------------===//

  const Token &tok() const { return tokens[index]; }

  /// True when the token after the current `[` is `]`.
  bool peekIsClose() const {
    return index + 1 < tokens.size() &&
           tokens[index + 1].is(TokenKind::RSquare);
  }

  const Token &consume() {
    const Token &token = tokens[index];
    if (!token.is(TokenKind::Eof)) {
      lastEnd = token.range.end;
      ++index;
    }
    return token;
  }

  //===--------------------------------------------------------------------===//
  // Diagnostics
  //===--------------------------------------------------------------------===//

  InFlightDiagnostic error(SourcePos pos, const Twine &message) {
    failed = true;
    lastErrorOffset = pos.offset;
    return file.emitError(pos, message);
  }

  /// Reports at the current token. Lexer error tokens were already reported,
  /// and a second error at the same token is suppressed.
  void errorAtToken(const Twine &message) {
    failed = true;
    if (tok().is(TokenKind::Error) ||
        tok().range.begin.offset == lastErrorOffset)
      return;
    error(tok().range.begin, message);
  }

  /// Reported just after the last token, where the bracket is missing.
  void errorUnclosed(char close, SourcePos open) {
    if (tok().range.begin.offset == lastErrorOffset) {
      failed = true;
      return;
    }
    error(lastEnd, "expected '" + Twine(close) + "'")
            .attachNote(file.getLoc(open))
        << "to match this '" << (close == '}' ? '{' : '[') << "'";
  }

  //===--------------------------------------------------------------------===//
  // Generic JSON structure
  //===--------------------------------------------------------------------===//

  /// Skips to the next `,` or `close` at the current nesting level (or EOF),
  /// without consuming it.
  void skipToSeparator(TokenKind close) {
    unsigned depth = 0;
    while (!tok().is(TokenKind::Eof)) {
      TokenKind kind = tok().kind;
      if (depth == 0 && (kind == TokenKind::Comma || kind == close))
        return;
      if (kind == TokenKind::LBrace || kind == TokenKind::LSquare)
        ++depth;
      else if ((kind == TokenKind::RBrace || kind == TokenKind::RSquare) &&
               depth > 0)
        --depth;
      consume();
    }
  }

  /// After a member or element: accepts `,` (returning true to continue) or
  /// `close` (returning false). Recovers from anything else.
  bool parseSeparator(TokenKind close, SourcePos open, StringRef what) {
    char closeChar = close == TokenKind::RBrace ? '}' : ']';
    while (true) {
      if (tok().is(TokenKind::Comma)) {
        SourcePos comma = consume().range.begin;
        if (!tok().is(close))
          return true;
        error(comma, "trailing comma is not allowed in JSON");
        consume();
        return false;
      }
      if (tok().is(close)) {
        consume();
        return false;
      }
      if (tok().is(TokenKind::Eof)) {
        errorUnclosed(closeChar, open);
        return false;
      }
      errorAtToken("expected ',' or '" + Twine(closeChar) + "' after " + what);
      skipToSeparator(close);
    }
  }

  /// `{ "key": value, ... }`. `onMember` is called with the current token at
  /// the value and must consume exactly one value.
  void parseObject(MemberFn onMember) {
    SourcePos open = consume().range.begin;
    if (tok().is(TokenKind::RBrace)) {
      consume();
      return;
    }
    do {
      if (tok().is(TokenKind::Eof)) {
        errorUnclosed('}', open);
        return;
      }
      if (!tok().is(TokenKind::String)) {
        errorAtToken("expected a string key");
        skipToSeparator(TokenKind::RBrace);
        continue;
      }
      const Token &key = consume();
      if (!tok().is(TokenKind::Colon)) {
        errorAtToken("expected ':' after object key");
        skipToSeparator(TokenKind::RBrace);
        continue;
      }
      consume();
      onMember(key);
    } while (parseSeparator(TokenKind::RBrace, open, "object member"));
  }

  /// `[ value, ... ]`. `onElement` must consume exactly one value.
  void parseArray(ElementFn onElement) {
    SourcePos open = consume().range.begin;
    if (tok().is(TokenKind::RSquare)) {
      consume();
      return;
    }
    do {
      if (tok().is(TokenKind::Eof)) {
        errorUnclosed(']', open);
        return;
      }
      onElement();
    } while (parseSeparator(TokenKind::RSquare, open, "array element"));
  }

  /// Parses and discards any JSON value, still reporting syntax errors.
  void skipValue(unsigned depth) {
    switch (tok().kind) {
    case TokenKind::String:
    case TokenKind::Number:
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::Null:
      consume();
      return;
    case TokenKind::Error:
      failed = true;
      consume();
      return;
    case TokenKind::LBrace:
    case TokenKind::LSquare:
      if (depth >= kMaxDepth) {
        errorAtToken("JSON nesting is deeper than " + Twine(kMaxDepth) +
                     " levels");
        skipNested();
        return;
      }
      if (tok().is(TokenKind::LBrace))
        parseObject([&](const Token &) { skipValue(depth + 1); });
      else
        parseArray([&] { skipValue(depth + 1); });
      return;
    case TokenKind::RBrace:
    case TokenKind::RSquare:
    case TokenKind::Comma:
    case TokenKind::Eof:
      errorAtToken("expected a JSON value");
      return;
    case TokenKind::Colon:
      errorAtToken("expected a JSON value");
      consume();
      return;
    }
  }

  /// Skips a bracketed value without recursion.
  void skipNested() {
    unsigned depth = 0;
    do {
      TokenKind kind = tok().kind;
      if (kind == TokenKind::LBrace || kind == TokenKind::LSquare)
        ++depth;
      else if (kind == TokenKind::RBrace || kind == TokenKind::RSquare)
        --depth;
      consume();
    } while (depth > 0 && !tok().is(TokenKind::Eof));
  }

  //===--------------------------------------------------------------------===//
  // Keyword values
  //===--------------------------------------------------------------------===//

  /// Reports that `keyword`'s value has the wrong shape and skips it.
  void badValue(const Token &key, const Twine &expected, unsigned depth) {
    errorAtToken("'" + key.value + "' must be " + expected);
    skipValue(depth);
  }

  bool parseNumber(const Token &key, double &value, std::string &spelling,
                   unsigned depth) {
    if (!tok().is(TokenKind::Number)) {
      bool draft4 = (key.value == "exclusiveMinimum" ||
                     key.value == "exclusiveMaximum") &&
                    (tok().is(TokenKind::True) || tok().is(TokenKind::False));
      if (draft4)
        badValue(key,
                 "a number in Draft 2020-12 (the boolean form is from "
                 "Draft 4)",
                 depth);
      else
        badValue(key, "a number", depth);
      return false;
    }
    const Token &number = consume();
    // `getAsDouble` returns true on failure; with `AllowInexact` an
    // overflow still succeeds and yields infinity.
    if (number.spelling.getAsDouble(value, /*AllowInexact=*/true) ||
        !std::isfinite(value)) {
      error(number.range.begin, "number is out of range");
      return false;
    }
    spelling = number.spelling.str();
    return true;
  }

  std::unique_ptr<KeywordNode> parseTypeKeyword(const Token &key,
                                                unsigned depth) {
    auto node = std::make_unique<TypeKeyword>(nextId++, key.range);
    auto addType = [&]() {
      const Token &name = consume();
      std::optional<JsonType> type = symbolizeJsonType(name.value);
      if (!type) {
        error(name.range.begin, "unknown type '" + name.value +
                                    "'; expected one of null, boolean, "
                                    "object, array, number, string, integer");
        return;
      }
      for (const Located<JsonType> &seen : node->types) {
        if (seen.value == *type) {
          error(name.range.begin,
                "type '" + name.value + "' is listed more than once");
          return;
        }
      }
      node->types.push_back({*type, name.range});
    };

    if (tok().is(TokenKind::String)) {
      addType();
    } else if (tok().is(TokenKind::LSquare)) {
      SourcePos open = tok().range.begin;
      bool empty = peekIsClose();
      parseArray([&] {
        if (tok().is(TokenKind::String))
          addType();
        else
          badValue(key, "a type name or an array of type names", depth + 1);
      });
      if (empty)
        error(open, "'type' must list at least one type");
    } else {
      badValue(key, "a type name or an array of type names", depth);
      return nullptr;
    }
    return node;
  }

  std::unique_ptr<KeywordNode> parsePropertiesKeyword(const Token &key,
                                                      unsigned depth) {
    if (!tok().is(TokenKind::LBrace)) {
      badValue(key, "an object", depth);
      return nullptr;
    }
    auto node = std::make_unique<PropertiesKeyword>(nextId++, key.range);
    llvm::StringMap<SourcePos> seen;
    parseObject([&](const Token &name) {
      if (!checkUnique(seen, name, "property", depth))
        return;
      auto property =
          std::make_unique<PropertyNode>(nextId++, name.value, name.range);
      property->schema = parseSchema(depth + 1);
      property->setEnd(lastEnd);
      node->properties.push_back(std::move(property));
    });
    return node;
  }

  std::unique_ptr<KeywordNode> parseRequiredKeyword(const Token &key,
                                                    unsigned depth) {
    if (!tok().is(TokenKind::LSquare)) {
      badValue(key, "an array of strings", depth);
      return nullptr;
    }
    auto node = std::make_unique<RequiredKeyword>(nextId++, key.range);
    parseArray([&] {
      if (!tok().is(TokenKind::String)) {
        badValue(key, "an array of strings", depth + 1);
        return;
      }
      const Token &name = consume();
      for (const Located<std::string> &seen : node->names) {
        if (seen.value == name.value) {
          error(name.range.begin,
                "'required' lists '" + name.value + "' more than once");
          return;
        }
      }
      node->names.push_back({name.value, name.range});
    });
    return node;
  }

  std::unique_ptr<KeywordNode> parseAllOfKeyword(const Token &key,
                                                 unsigned depth) {
    if (!tok().is(TokenKind::LSquare)) {
      badValue(key, "a non-empty array of schemas", depth);
      return nullptr;
    }
    auto node = std::make_unique<AllOfKeyword>(nextId++, key.range);
    SourcePos open = tok().range.begin;
    bool empty = peekIsClose();
    parseArray([&] {
      if (std::unique_ptr<SchemaNode> branch = parseSchema(depth + 1))
        node->branches.push_back(std::move(branch));
    });
    if (empty)
      error(open, "'allOf' must contain at least one schema");
    return node;
  }

  std::unique_ptr<KeywordNode> parseKeyword(const Token &key, unsigned depth) {
    KeywordKind kind =
        llvm::StringSwitch<KeywordKind>(key.value)
            .Case("type", KeywordKind::Type)
            .Case("properties", KeywordKind::Properties)
            .Case("required", KeywordKind::Required)
            .Case("allOf", KeywordKind::AllOf)
            .Case("minimum", KeywordKind::Minimum)
            .Case("maximum", KeywordKind::Maximum)
            .Case("exclusiveMinimum", KeywordKind::ExclusiveMinimum)
            .Case("exclusiveMaximum", KeywordKind::ExclusiveMaximum)
            .Case("multipleOf", KeywordKind::MultipleOf)
            .Case("minLength", KeywordKind::MinLength)
            .Case("maxLength", KeywordKind::MaxLength)
            .Case("pattern", KeywordKind::Pattern)
            .Case("format", KeywordKind::Format)
            .Case("$schema", KeywordKind::Annotation)
            .Case("$id", KeywordKind::Annotation)
            .Case("$comment", KeywordKind::Annotation)
            .Case("title", KeywordKind::Annotation)
            .Case("description", KeywordKind::Annotation)
            .Case("default", KeywordKind::Annotation)
            .Case("examples", KeywordKind::Annotation)
            .Case("deprecated", KeywordKind::Annotation)
            .Case("readOnly", KeywordKind::Annotation)
            .Case("writeOnly", KeywordKind::Annotation)
            .Default(KeywordKind::Unknown);

    switch (kind) {
    case KeywordKind::Type:
      return parseTypeKeyword(key, depth);
    case KeywordKind::Properties:
      return parsePropertiesKeyword(key, depth);
    case KeywordKind::Required:
      return parseRequiredKeyword(key, depth);
    case KeywordKind::AllOf:
      return parseAllOfKeyword(key, depth);

    case KeywordKind::Minimum:
    case KeywordKind::Maximum:
    case KeywordKind::ExclusiveMinimum:
    case KeywordKind::ExclusiveMaximum:
    case KeywordKind::MultipleOf: {
      auto node =
          std::make_unique<NumberKeyword>(nextId++, kind, key.value, key.range);
      if (!parseNumber(key, node->value, node->spelling, depth))
        return nullptr;
      return node;
    }

    case KeywordKind::MinLength:
    case KeywordKind::MaxLength: {
      auto node =
          std::make_unique<LengthKeyword>(nextId++, kind, key.value, key.range);
      SourcePos at = tok().range.begin;
      double value;
      std::string spelling;
      if (!tok().is(TokenKind::Number)) {
        badValue(key, "a non-negative integer", depth);
        return nullptr;
      }
      if (!parseNumber(key, value, spelling, depth))
        return nullptr;
      // Draft 2020-12 counts `1.0` as an integer.
      if (value < 0 || value != std::trunc(value) || value > kMaxExactInteger) {
        error(at, "'" + key.value + "' must be a non-negative integer");
        return nullptr;
      }
      node->value = static_cast<int64_t>(value);
      return node;
    }

    case KeywordKind::Pattern:
    case KeywordKind::Format: {
      auto node =
          std::make_unique<StringKeyword>(nextId++, kind, key.value, key.range);
      if (!tok().is(TokenKind::String)) {
        badValue(key, "a string", depth);
        return nullptr;
      }
      node->value = consume().value;
      return node;
    }

    case KeywordKind::Annotation: {
      auto node =
          std::make_unique<AnnotationKeyword>(nextId++, key.value, key.range);
      if (tok().is(TokenKind::String))
        node->text = consume().value;
      else
        skipValue(depth);
      return node;
    }

    case KeywordKind::Unknown: {
      auto node =
          std::make_unique<UnknownKeyword>(nextId++, key.value, key.range);
      skipValue(depth);
      return node;
    }
    }
    llvm_unreachable("unknown keyword kind");
  }

  /// Records `key` in `seen`; on a duplicate, reports it, skips the value and
  /// returns false.
  bool checkUnique(llvm::StringMap<SourcePos> &seen, const Token &key,
                   StringRef what, unsigned depth) {
    auto [it, inserted] = seen.try_emplace(key.value, key.range.begin);
    if (inserted)
      return true;
    error(key.range.begin, "duplicate " + what + " '" + key.value + "'")
            .attachNote(file.getLoc(it->second))
        << "first defined here";
    skipValue(depth);
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Schemas
  //===--------------------------------------------------------------------===//

  std::unique_ptr<SchemaNode> parseSchema(unsigned depth) {
    if (depth >= kMaxDepth) {
      errorAtToken("schema nesting is deeper than " + Twine(kMaxDepth) +
                   " levels");
      skipNested();
      return nullptr;
    }
    if (tok().is(TokenKind::True) || tok().is(TokenKind::False)) {
      errorAtToken("boolean schemas are not supported; use an object schema");
      consume();
      return nullptr;
    }
    if (!tok().is(TokenKind::LBrace)) {
      errorAtToken("expected a schema object");
      skipValue(depth);
      return nullptr;
    }

    auto node = std::make_unique<SchemaNode>(nextId++, tok().range.begin);
    llvm::StringMap<SourcePos> seen;
    parseObject([&](const Token &key) {
      if (!checkUnique(seen, key, "key", depth))
        return;
      std::unique_ptr<KeywordNode> keyword = parseKeyword(key, depth);
      if (!keyword)
        return;
      keyword->setEnd(lastEnd);
      node->addKeyword(std::move(keyword));
    });
    node->setEnd(lastEnd);
    return node;
  }

  const SourceFile &file;
  ArrayRef<Token> tokens;
  size_t index = 0;
  SourcePos lastEnd;
  unsigned lastErrorOffset = ~0u;
  unsigned nextId = 0;
  bool failed = false;
};

ParsedSchema Parser::parseDocument() {
  ParsedSchema result;
  if (tok().is(TokenKind::Eof)) {
    error(tok().range.begin, "expected a JSON Schema document");
  } else {
    result.root = parseSchema(/*depth=*/0);
    if (!tok().is(TokenKind::Eof))
      errorAtToken("unexpected content after the schema");
  }
  result.numNodes = nextId;
  result.failed = failed;
  return result;
}

} // namespace

ParsedSchema mlir::schema::json::parseSchema(const SourceFile &file,
                                             ArrayRef<Token> tokens) {
  return Parser(file, tokens).parseDocument();
}
