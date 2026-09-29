//===- AST.cpp - JSON Schema abstract syntax tree -------------------------===//

#include "Schema/Import/AST.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Format.h"

using namespace mlir::schema::json;
using llvm::raw_ostream;
using llvm::StringRef;

StringRef mlir::schema::json::stringifyJsonType(JsonType type) {
  switch (type) {
  case JsonType::Null:
    return "null";
  case JsonType::Boolean:
    return "boolean";
  case JsonType::Object:
    return "object";
  case JsonType::Array:
    return "array";
  case JsonType::Number:
    return "number";
  case JsonType::String:
    return "string";
  case JsonType::Integer:
    return "integer";
  }
  llvm_unreachable("unknown JSON type");
}

std::optional<JsonType> mlir::schema::json::symbolizeJsonType(StringRef name) {
  return llvm::StringSwitch<std::optional<JsonType>>(name)
      .Case("null", JsonType::Null)
      .Case("boolean", JsonType::Boolean)
      .Case("object", JsonType::Object)
      .Case("array", JsonType::Array)
      .Case("number", JsonType::Number)
      .Case("string", JsonType::String)
      .Case("integer", JsonType::Integer)
      .Default(std::nullopt);
}

void SchemaNode::addKeyword(std::unique_ptr<KeywordNode> keyword) {
  keywords.push_back(std::move(keyword));
}

std::optional<StringRef> SchemaNode::getTitle() const {
  for (const std::unique_ptr<KeywordNode> &keyword : keywords)
    if (auto *annotation = llvm::dyn_cast<AnnotationKeyword>(keyword.get()))
      if (annotation->getName() == "title" && annotation->text)
        return StringRef(*annotation->text);
  return std::nullopt;
}

std::vector<const Node *> mlir::schema::json::getChildren(const Node &node) {
  std::vector<const Node *> children;
  llvm::TypeSwitch<const Node *>(&node)
      .Case([&](const SchemaNode *schema) {
        for (const std::unique_ptr<KeywordNode> &keyword :
             schema->getKeywords())
          children.push_back(keyword.get());
      })
      .Case([&](const PropertyNode *property) {
        if (property->schema)
          children.push_back(property->schema.get());
      })
      .Case([&](const PropertiesKeyword *properties) {
        for (const std::unique_ptr<PropertyNode> &property :
             properties->properties)
          children.push_back(property.get());
      })
      .Case([&](const AllOfKeyword *allOf) {
        for (const std::unique_ptr<SchemaNode> &branch : allOf->branches)
          children.push_back(branch.get());
      });
  return children;
}

void mlir::schema::json::walk(const Node &node,
                              llvm::function_ref<void(const Node &)> fn) {
  fn(node);
  for (const Node *child : getChildren(node))
    walk(*child, fn);
}

//===----------------------------------------------------------------------===//
// --dump-ast
//===----------------------------------------------------------------------===//

/// Prints `value` as a JSON string literal.
static void printQuoted(StringRef value, raw_ostream &os) {
  os << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      os << '\\' << c;
    else if (c == '\n')
      os << "\\n";
    else if (c < 0x20)
      os << llvm::format("\\u%04x", c);
    else
      os << c;
  }
  os << '"';
}

static void printRange(const SourceRange &range, raw_ostream &os) {
  os << '[' << range.begin.line << ':' << range.begin.column << '-'
     << range.end.line << ':' << range.end.column << ']';
}

static void dumpNode(const Node &node, unsigned depth, raw_ostream &os) {
  os.indent(2 * depth);
  llvm::TypeSwitch<const Node *>(&node)
      .Case([&](const SchemaNode *) { os << "schema"; })
      .Case([&](const PropertyNode *property) {
        os << "property ";
        printQuoted(property->getName(), os);
      })
      .Case([&](const KeywordNode *keyword) { os << keyword->getName(); });
  os << " #" << node.getId() << ' ';
  printRange(node.getRange(), os);

  llvm::TypeSwitch<const Node *>(&node)
      .Case([&](const TypeKeyword *type) {
        os << ' ';
        if (type->types.size() == 1) {
          os << stringifyJsonType(type->types.front().value);
          return;
        }
        os << '[';
        llvm::interleaveComma(type->types, os,
                              [&](const Located<JsonType> &entry) {
                                os << stringifyJsonType(entry.value);
                              });
        os << ']';
      })
      .Case([&](const RequiredKeyword *required) {
        os << " [";
        llvm::interleaveComma(required->names, os,
                              [&](const Located<std::string> &name) {
                                printQuoted(name.value, os);
                              });
        os << ']';
      })
      .Case([&](const NumberKeyword *number) { os << ' ' << number->spelling; })
      .Case([&](const LengthKeyword *length) { os << ' ' << length->value; })
      .Case([&](const StringKeyword *string) {
        os << ' ';
        printQuoted(string->value, os);
      })
      .Case([&](const AnnotationKeyword *annotation) {
        if (annotation->text) {
          os << ' ';
          printQuoted(*annotation->text, os);
        }
      })
      .Case([&](const UnknownKeyword *) { os << " <unsupported>"; });
  os << '\n';

  for (const Node *child : getChildren(node))
    dumpNode(*child, depth + 1, os);
}

void mlir::schema::json::dumpAst(const SchemaNode &root, raw_ostream &os) {
  dumpNode(root, 0, os);
}
