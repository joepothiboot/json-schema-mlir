//===- AST.h - JSON Schema abstract syntax tree -----------------*- C++ -*-===//
//
// The parser's output: one `SchemaNode` per schema object, holding its
// keywords in source order. Every node has a pre-order id (the root is 0)
// and the source range it was parsed from. Nodes use LLVM-style RTTI
// (`classof`), so they work with `dyn_cast` and `TypeSwitch`.
//
//===----------------------------------------------------------------------===//

#ifndef SCHEMA_IMPORT_AST_H
#define SCHEMA_IMPORT_AST_H

#include "Schema/Import/Lexer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mlir::schema::json {

/// A value together with the source range it was read from.
template <typename T> struct Located {
  T value;
  SourceRange range;
};

/// The JSON Schema `type` names (Draft 2020-12, §6.1.1).
enum class JsonType { Null, Boolean, Object, Array, Number, String, Integer };

StringRef stringifyJsonType(JsonType type);
std::optional<JsonType> symbolizeJsonType(StringRef name);

/// The keywords the parser understands. `Annotation` covers keywords with no
/// effect on validation (`title`, `$comment`, ...); `Unknown` is any other
/// key, kept in the AST so the importer can report it.
enum class KeywordKind {
  Type,
  Properties,
  Required,
  AllOf,
  Minimum,
  Maximum,
  ExclusiveMinimum,
  ExclusiveMaximum,
  MultipleOf,
  MinLength,
  MaxLength,
  Pattern,
  Format,
  Annotation,
  Unknown,
};

class Node {
public:
  enum class Kind {
    Schema,
    Property,
    // Keywords.
    TypeKeyword,
    PropertiesKeyword,
    RequiredKeyword,
    AllOfKeyword,
    NumberKeyword,
    LengthKeyword,
    StringKeyword,
    AnnotationKeyword,
    UnknownKeyword,
  };

  virtual ~Node() = default;

  Kind getKind() const { return kind; }
  unsigned getId() const { return id; }
  const SourceRange &getRange() const { return range; }
  void setEnd(SourcePos end) { range.end = end; }

protected:
  Node(Kind kind, unsigned id, SourcePos begin)
      : kind(kind), id(id), range{begin, begin} {}

private:
  Kind kind;
  unsigned id;
  SourceRange range;
};

class KeywordNode;

/// A JSON Schema object: `{ "type": ..., "minimum": ..., ... }`.
class SchemaNode : public Node {
public:
  SchemaNode(unsigned id, SourcePos begin) : Node(Kind::Schema, id, begin) {}

  const std::vector<std::unique_ptr<KeywordNode>> &getKeywords() const {
    return keywords;
  }
  void addKeyword(std::unique_ptr<KeywordNode> keyword);

  /// The first keyword of kind `T` (and `kind`, when given), or null.
  template <typename T>
  const T *find(std::optional<KeywordKind> kind = std::nullopt) const;

  /// The string value of the `title` annotation, if any.
  std::optional<StringRef> getTitle() const;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::Schema;
  }

private:
  std::vector<std::unique_ptr<KeywordNode>> keywords;
};

/// Base class of every keyword. The node's range covers `"key": value`;
/// `getKeyRange()` is just the key.
class KeywordNode : public Node {
public:
  KeywordKind getKeywordKind() const { return keywordKind; }
  StringRef getName() const { return name; }
  const SourceRange &getKeyRange() const { return keyRange; }

  static bool classof(const Node *node) {
    return node->getKind() >= Kind::TypeKeyword &&
           node->getKind() <= Kind::UnknownKeyword;
  }

protected:
  KeywordNode(Kind kind, unsigned id, KeywordKind keywordKind, StringRef name,
              SourceRange keyRange)
      : Node(kind, id, keyRange.begin), keywordKind(keywordKind),
        name(name.str()), keyRange(keyRange) {}

private:
  KeywordKind keywordKind;
  std::string name;
  SourceRange keyRange;
};

/// `"type": "string"` or `"type": ["string", "null"]`.
class TypeKeyword : public KeywordNode {
public:
  TypeKeyword(unsigned id, SourceRange keyRange)
      : KeywordNode(Kind::TypeKeyword, id, KeywordKind::Type, "type",
                    keyRange) {}

  std::vector<Located<JsonType>> types;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::TypeKeyword;
  }
};

/// One member of `properties`: `"name": { ...schema... }`.
class PropertyNode : public Node {
public:
  PropertyNode(unsigned id, StringRef name, SourceRange nameRange)
      : Node(Kind::Property, id, nameRange.begin), name(name.str()),
        nameRange(nameRange) {}

  StringRef getName() const { return name; }
  const SourceRange &getNameRange() const { return nameRange; }

  std::unique_ptr<SchemaNode> schema;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::Property;
  }

private:
  std::string name;
  SourceRange nameRange;
};

class PropertiesKeyword : public KeywordNode {
public:
  PropertiesKeyword(unsigned id, SourceRange keyRange)
      : KeywordNode(Kind::PropertiesKeyword, id, KeywordKind::Properties,
                    "properties", keyRange) {}

  std::vector<std::unique_ptr<PropertyNode>> properties;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::PropertiesKeyword;
  }
};

class RequiredKeyword : public KeywordNode {
public:
  RequiredKeyword(unsigned id, SourceRange keyRange)
      : KeywordNode(Kind::RequiredKeyword, id, KeywordKind::Required,
                    "required", keyRange) {}

  std::vector<Located<std::string>> names;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::RequiredKeyword;
  }
};

class AllOfKeyword : public KeywordNode {
public:
  AllOfKeyword(unsigned id, SourceRange keyRange)
      : KeywordNode(Kind::AllOfKeyword, id, KeywordKind::AllOf, "allOf",
                    keyRange) {}

  std::vector<std::unique_ptr<SchemaNode>> branches;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::AllOfKeyword;
  }
};

/// `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum` and
/// `multipleOf`.
class NumberKeyword : public KeywordNode {
public:
  NumberKeyword(unsigned id, KeywordKind kind, StringRef name,
                SourceRange keyRange)
      : KeywordNode(Kind::NumberKeyword, id, kind, name, keyRange) {}

  double value = 0.0;
  /// The number as written, for diagnostics.
  std::string spelling;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::NumberKeyword;
  }
};

/// `minLength` and `maxLength`.
class LengthKeyword : public KeywordNode {
public:
  LengthKeyword(unsigned id, KeywordKind kind, StringRef name,
                SourceRange keyRange)
      : KeywordNode(Kind::LengthKeyword, id, kind, name, keyRange) {}

  int64_t value = 0;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::LengthKeyword;
  }
};

/// `pattern` and `format`.
class StringKeyword : public KeywordNode {
public:
  StringKeyword(unsigned id, KeywordKind kind, StringRef name,
                SourceRange keyRange)
      : KeywordNode(Kind::StringKeyword, id, kind, name, keyRange) {}

  std::string value;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::StringKeyword;
  }
};

/// `title`, `description`, `$schema`, `$comment`, `default`, ...
class AnnotationKeyword : public KeywordNode {
public:
  AnnotationKeyword(unsigned id, StringRef name, SourceRange keyRange)
      : KeywordNode(Kind::AnnotationKeyword, id, KeywordKind::Annotation, name,
                    keyRange) {}

  /// Set when the value is a JSON string.
  std::optional<std::string> text;

  static bool classof(const Node *node) {
    return node->getKind() == Kind::AnnotationKeyword;
  }
};

/// A key the front end does not support. Its value is skipped.
class UnknownKeyword : public KeywordNode {
public:
  UnknownKeyword(unsigned id, StringRef name, SourceRange keyRange)
      : KeywordNode(Kind::UnknownKeyword, id, KeywordKind::Unknown, name,
                    keyRange) {}

  static bool classof(const Node *node) {
    return node->getKind() == Kind::UnknownKeyword;
  }
};

template <typename T>
const T *SchemaNode::find(std::optional<KeywordKind> kind) const {
  for (const std::unique_ptr<KeywordNode> &keyword : keywords)
    if (auto *typed = llvm::dyn_cast<T>(keyword.get()))
      if (!kind || typed->getKeywordKind() == *kind)
        return typed;
  return nullptr;
}

/// Calls `fn` on every node in pre-order (i.e. in id order).
void walk(const Node &node, llvm::function_ref<void(const Node &)> fn);

/// The children of `node` in source order.
std::vector<const Node *> getChildren(const Node &node);

/// Prints the indented tree used by `--dump-ast`.
void dumpAst(const SchemaNode &root, llvm::raw_ostream &os);

} // namespace mlir::schema::json

#endif // SCHEMA_IMPORT_AST_H
