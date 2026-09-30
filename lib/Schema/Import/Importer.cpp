//===- Importer.cpp - Schema AST to `schema` dialect ----------------------===//
//
// Lowers the schema AST to one `schema` op per assertion keyword and joins
// them with `arith.andi`, exactly the naive form `--schema-canonicalize` is
// designed to clean up. Keeping one op per keyword means every op's location
// names a single JSON keyword.
//
// Within one schema object the conjuncts are emitted in a fixed order:
//
//   1. the type guard for a scalar `type` (string / number / integer),
//   2. string and number assertions, in source order,
//   3. `schema.struct` for `type: object` / `properties` / `required`,
//   4. each `allOf` branch.
//
// The dialect's validators check the JSON type as well as the constraint, so
// `{"minimum": 0}` alone (which accepts any non-number) cannot be expressed.
// A keyword therefore needs a `type` from its own schema or from the `allOf`
// conjunction it belongs to.
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Importer.h"

#include "Schema/SchemaDialect.h"
#include "Schema/SchemaOps.h"
#include "Schema/SchemaTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"

using namespace mlir;
using namespace mlir::schema;
using namespace mlir::schema::json;

namespace {

/// `OpBuilder::create<OpTy>` is deprecated in favour of `OpTy::create` from
/// LLVM 21 on; the latter does not exist before that.
template <typename OpTy, typename... Args>
OpTy createOp(OpBuilder &builder, Location loc, Args &&...args) {
#if LLVM_VERSION_MAJOR >= 21
  return OpTy::create(builder, loc, std::forward<Args>(args)...);
#else
  return builder.create<OpTy>(loc, std::forward<Args>(args)...);
#endif
}

/// The instance type a conjunction of schemas guarantees. `Integer` is the
/// subtype of `Number`.
enum class InstanceKind { Any, String, Number, Integer, Object };

StringRef describe(InstanceKind kind) {
  switch (kind) {
  case InstanceKind::Any:
    return "any value";
  case InstanceKind::String:
    return "strings";
  case InstanceKind::Number:
    return "numbers";
  case InstanceKind::Integer:
    return "integers";
  case InstanceKind::Object:
    return "objects";
  }
  llvm_unreachable("unknown instance kind");
}

struct KindFact {
  InstanceKind kind = InstanceKind::Any;
  /// The `type` entry that established `kind`, for notes.
  const Located<JsonType> *origin = nullptr;
};

/// Escapes a property name as a JSON Pointer reference token (RFC 6901).
std::string escapePointerToken(StringRef name) {
  std::string out;
  for (char c : name) {
    if (c == '~')
      out += "~0";
    else if (c == '/')
      out += "~1";
    else
      out += c;
  }
  return out;
}

/// `validate_` + the lower-cased alphanumeric words of `name`.
std::string getFunctionName(StringRef name) {
  std::string out = "validate";
  bool separate = true;
  for (char c : name) {
    if (!llvm::isAlnum(c)) {
      separate = true;
      continue;
    }
    if (separate)
      out += '_';
    separate = false;
    out += llvm::toLower(c);
  }
  return out;
}

class Importer {
public:
  explicit Importer(const SourceFile &file)
      : file(file), context(file.getContext()), builder(context) {}

  OwningOpRef<ModuleOp> run(const SchemaNode &root);

private:
  //===--------------------------------------------------------------------===//
  // Helpers
  //===--------------------------------------------------------------------===//

  Location loc(const Node &node) const {
    return file.getLoc(node.getRange().begin);
  }

  InFlightDiagnostic error(SourcePos pos, const Twine &message) {
    failed = true;
    return file.emitError(pos, message);
  }
  InFlightDiagnostic error(const Node &node, const Twine &message) {
    return error(node.getRange().begin, message);
  }

  Value getOrCreateArgument(StringRef path, Location argLoc) {
    auto [it, inserted] = arguments.try_emplace(path, Value());
    if (inserted)
      it->second = body->addArgument(ValueType::get(context), argLoc);
    return it->second;
  }

  Value createTrue(Location at) {
    return createOp<arith::ConstantOp>(builder, at, builder.getBoolAttr(true));
  }

  Value createNumberOp(Location at, Value input, FloatAttr minimum,
                       FloatAttr maximum, FloatAttr multipleOf,
                       bool exclusiveMin, bool exclusiveMax, bool integral) {
    UnitAttr unit = builder.getUnitAttr();
    return createOp<ValidateNumberOp>(
        builder, at, builder.getI1Type(), input, minimum, maximum, multipleOf,
        exclusiveMin ? unit : UnitAttr(), exclusiveMax ? unit : UnitAttr(),
        integral ? unit : UnitAttr());
  }

  Value createStringOp(Location at, Value input, IntegerAttr minLength,
                       IntegerAttr maxLength, StringAttr pattern,
                       StringAttr format) {
    return createOp<ValidateStringOp>(builder, at, builder.getI1Type(), input,
                                      minLength, maxLength, pattern, format);
  }

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  KindFact collectKind(const SchemaNode &node, KindFact fact);

  /// Checks that `keyword` applies to the conjunction's instance kind.
  /// Returns false (after an error, or a warning when the keyword is merely
  /// vacuous) if no op should be emitted for it.
  bool checkApplies(const KeywordNode &keyword, KindFact fact,
                    ArrayRef<InstanceKind> accepted, StringRef required);

  //===--------------------------------------------------------------------===//
  // Schemas
  //===--------------------------------------------------------------------===//

  Value importSchema(const SchemaNode &node, Value input, KindFact fact,
                     StringRef path, StringRef typeName);
  Value importNumberKeyword(const NumberKeyword &keyword, Value input);
  Value importStringKeyword(const KeywordNode &keyword, Value input);
  Value importObject(const SchemaNode &node, const KeywordNode &anchor,
                     Value input, StringRef path, StringRef typeName);
  void checkAnnotation(const AnnotationKeyword &keyword);
  void checkBounds(const SchemaNode &node);

  const SourceFile &file;
  MLIRContext *context;
  OpBuilder builder;
  Block *body = nullptr;
  /// One function argument per instance path (JSON Pointer).
  llvm::StringMap<Value> arguments;
  bool failed = false;
};

} // namespace

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

KindFact Importer::collectKind(const SchemaNode &node, KindFact fact) {
  if (const auto *type = node.find<TypeKeyword>()) {
    if (type->types.size() != 1) {
      error(*type, "type unions are not supported; 'type' must name exactly "
                   "one type");
    } else {
      const Located<JsonType> &entry = type->types.front();
      std::optional<InstanceKind> own;
      switch (entry.value) {
      case JsonType::String:
        own = InstanceKind::String;
        break;
      case JsonType::Number:
        own = InstanceKind::Number;
        break;
      case JsonType::Integer:
        own = InstanceKind::Integer;
        break;
      case JsonType::Object:
        own = InstanceKind::Object;
        break;
      case JsonType::Null:
      case JsonType::Boolean:
      case JsonType::Array:
        error(entry.range.begin, "type '" + stringifyJsonType(entry.value) +
                                     "' is not supported by the schema "
                                     "dialect");
        break;
      }

      if (own) {
        InstanceKind have = fact.kind;
        if (have == InstanceKind::Any || have == *own ||
            (have == InstanceKind::Number && *own == InstanceKind::Integer)) {
          fact = {*own, &entry};
        } else if (have == InstanceKind::Integer &&
                   *own == InstanceKind::Number) {
          // `integer` already implies `number`.
        } else {
          error(entry.range.begin, "type '" + stringifyJsonType(entry.value) +
                                       "' contradicts type '" +
                                       stringifyJsonType(fact.origin->value) +
                                       "'; no value satisfies both")
                  .attachNote(file.getLoc(fact.origin->range.begin))
              << "'" << stringifyJsonType(fact.origin->value)
              << "' is required here";
        }
      }
    }
  }

  // `allOf` is a conjunction: a type asserted by any branch holds for the
  // instance as a whole.
  if (const auto *allOf = node.find<AllOfKeyword>())
    for (const std::unique_ptr<SchemaNode> &branch : allOf->branches)
      fact = collectKind(*branch, fact);
  return fact;
}

bool Importer::checkApplies(const KeywordNode &keyword, KindFact fact,
                            ArrayRef<InstanceKind> accepted,
                            StringRef required) {
  if (llvm::is_contained(accepted, fact.kind))
    return true;
  if (fact.kind == InstanceKind::Any) {
    error(keyword, "'" + keyword.getName() + "' requires " + required +
                       " on this schema or in an 'allOf' with it");
    return false;
  }
  // JSON Schema ignores an assertion that does not apply to the instance's
  // type, so this is legal but almost certainly a mistake.
  file.emitWarning(keyword.getRange().begin, "'" + keyword.getName() +
                                                 "' has no effect: this schema "
                                                 "only accepts " +
                                                 describe(fact.kind));
  return false;
}

//===----------------------------------------------------------------------===//
// Keywords
//===----------------------------------------------------------------------===//

Value Importer::importNumberKeyword(const NumberKeyword &keyword, Value input) {
  FloatAttr value = builder.getF64FloatAttr(keyword.value);
  Location at = loc(keyword);
  switch (keyword.getKeywordKind()) {
  case KeywordKind::Minimum:
    return createNumberOp(at, input, value, {}, {}, false, false, false);
  case KeywordKind::ExclusiveMinimum:
    return createNumberOp(at, input, value, {}, {}, true, false, false);
  case KeywordKind::Maximum:
    return createNumberOp(at, input, {}, value, {}, false, false, false);
  case KeywordKind::ExclusiveMaximum:
    return createNumberOp(at, input, {}, value, {}, false, true, false);
  case KeywordKind::MultipleOf:
    return createNumberOp(at, input, {}, {}, value, false, false, false);
  default:
    llvm_unreachable("not a number keyword");
  }
}

Value Importer::importStringKeyword(const KeywordNode &keyword, Value input) {
  Location at = loc(keyword);
  if (const auto *length = llvm::dyn_cast<LengthKeyword>(&keyword)) {
    IntegerAttr value = builder.getI64IntegerAttr(length->value);
    if (keyword.getKeywordKind() == KeywordKind::MinLength)
      return createStringOp(at, input, value, {}, {}, {});
    return createStringOp(at, input, {}, value, {}, {});
  }

  const auto &string = llvm::cast<StringKeyword>(keyword);
  if (keyword.getKeywordKind() == KeywordKind::Pattern) {
    // An empty pattern matches every string.
    if (string.value.empty())
      return {};
    std::string message;
    if (!llvm::Regex(string.value).isValid(message)) {
      error(keyword, "'pattern' is not a valid regular expression: " + message);
      return {};
    }
    return createStringOp(at, input, {}, {},
                          builder.getStringAttr(string.value), {});
  }

  if (string.value.empty()) {
    error(keyword, "'format' must not be empty");
    return {};
  }
  return createStringOp(at, input, {}, {}, {},
                        builder.getStringAttr(string.value));
}

void Importer::checkAnnotation(const AnnotationKeyword &keyword) {
  if (keyword.getName() != "$schema" || !keyword.text)
    return;
  StringRef uri = *keyword.text;
  uri.consume_back("#");
  if (uri != "https://json-schema.org/draft/2020-12/schema")
    error(keyword, "unsupported '$schema' '" + *keyword.text +
                       "'; only Draft 2020-12 is supported");
}

void Importer::checkBounds(const SchemaNode &node) {
  const NumberKeyword *lowers[] = {
      node.find<NumberKeyword>(KeywordKind::Minimum),
      node.find<NumberKeyword>(KeywordKind::ExclusiveMinimum)};
  const NumberKeyword *uppers[] = {
      node.find<NumberKeyword>(KeywordKind::Maximum),
      node.find<NumberKeyword>(KeywordKind::ExclusiveMaximum)};
  for (const NumberKeyword *lower : lowers) {
    for (const NumberKeyword *upper : uppers) {
      if (!lower || !upper)
        continue;
      bool exclusive =
          lower->getKeywordKind() == KeywordKind::ExclusiveMinimum ||
          upper->getKeywordKind() == KeywordKind::ExclusiveMaximum;
      if (lower->value < upper->value ||
          (lower->value == upper->value && !exclusive))
        continue;
      error(*lower, "no number satisfies both '" + lower->getName() + "' (" +
                        lower->spelling + ") and '" + upper->getName() + "' (" +
                        upper->spelling + ")")
              .attachNote(loc(*upper))
          << "'" << upper->getName() << "' is here";
    }
  }

  const auto *minLength = node.find<LengthKeyword>(KeywordKind::MinLength);
  const auto *maxLength = node.find<LengthKeyword>(KeywordKind::MaxLength);
  if (minLength && maxLength && minLength->value > maxLength->value)
    error(*minLength, "no string satisfies both 'minLength' (" +
                          Twine(minLength->value) + ") and 'maxLength' (" +
                          Twine(maxLength->value) + ")")
            .attachNote(loc(*maxLength))
        << "'maxLength' is here";

  if (const auto *multipleOf =
          node.find<NumberKeyword>(KeywordKind::MultipleOf))
    if (!(multipleOf->value > 0))
      error(*multipleOf,
            "'multipleOf' must be greater than 0, got " + multipleOf->spelling);
}

//===----------------------------------------------------------------------===//
// Schemas
//===----------------------------------------------------------------------===//

Value Importer::importSchema(const SchemaNode &node, Value input, KindFact fact,
                             StringRef path, StringRef typeName) {
  SmallVector<Value> conjuncts;
  auto add = [&](Value value) {
    if (value)
      conjuncts.push_back(value);
  };

  // 1. Type guard.
  const auto *type = node.find<TypeKeyword>();
  bool isObject = false;
  if (type && type->types.size() == 1) {
    Location at = loc(*type);
    switch (type->types.front().value) {
    case JsonType::String:
      add(createStringOp(at, input, {}, {}, {}, {}));
      break;
    case JsonType::Number:
      add(createNumberOp(at, input, {}, {}, {}, false, false, false));
      break;
    case JsonType::Integer:
      add(createNumberOp(at, input, {}, {}, {}, false, false, true));
      break;
    case JsonType::Object:
      isObject = true;
      break;
    default:
      break; // Reported by collectKind.
    }
  }

  // 2. Scalar assertions, in source order.
  checkBounds(node);
  static constexpr InstanceKind numeric[] = {InstanceKind::Number,
                                             InstanceKind::Integer};
  for (const std::unique_ptr<KeywordNode> &keyword : node.getKeywords()) {
    llvm::TypeSwitch<const KeywordNode *>(keyword.get())
        .Case([&](const NumberKeyword *number) {
          if (checkApplies(*number, fact, numeric,
                           "\"type\": \"number\" or \"integer\""))
            add(importNumberKeyword(*number, input));
        })
        .Case<LengthKeyword, StringKeyword>([&](const KeywordNode *string) {
          if (checkApplies(*string, fact, {InstanceKind::String},
                           "\"type\": \"string\""))
            add(importStringKeyword(*string, input));
        })
        .Case([&](const AnnotationKeyword *annotation) {
          checkAnnotation(*annotation);
        })
        .Case([&](const UnknownKeyword *unknown) {
          error(*unknown, "keyword '" + unknown->getName() +
                              "' is not supported by the schema front end");
        });
  }

  // 3. Object structure.
  const auto *properties = node.find<PropertiesKeyword>();
  const auto *required = node.find<RequiredKeyword>();
  if (isObject || properties || required) {
    const KeywordNode *anchor = properties;
    if (!anchor)
      anchor = isObject ? static_cast<const KeywordNode *>(type) : required;
    if (isObject || checkApplies(*anchor, fact, {InstanceKind::Object},
                                 "\"type\": \"object\""))
      add(importObject(node, *anchor, input, path, typeName));
  }

  // 4. allOf branches, which share this schema's instance.
  if (const auto *allOf = node.find<AllOfKeyword>())
    for (const std::unique_ptr<SchemaNode> &branch : allOf->branches)
      add(importSchema(*branch, input, fact, path, typeName));

  if (conjuncts.empty())
    return createTrue(loc(node));
  Value verdict = conjuncts.front();
  for (Value conjunct : llvm::drop_begin(conjuncts))
    verdict = createOp<arith::AndIOp>(builder, loc(node), verdict, conjunct);
  return verdict;
}

Value Importer::importObject(const SchemaNode &node, const KeywordNode &anchor,
                             Value input, StringRef path, StringRef typeName) {
  const auto *properties = node.find<PropertiesKeyword>();
  SmallVector<Attribute> names;
  SmallVector<Value> verdicts;
  llvm::StringSet<> declared;

  if (properties) {
    for (const std::unique_ptr<PropertyNode> &property :
         properties->properties) {
      if (!property->schema)
        continue;
      if (property->getName().empty()) {
        error(*property, "empty property names are not supported");
        continue;
      }
      declared.insert(property->getName());
      std::string childPath =
          (path + "/" + escapePointerToken(property->getName())).str();
      Value argument = getOrCreateArgument(childPath, loc(*property));
      const SchemaNode &schema = *property->schema;
      Value verdict =
          importSchema(schema, argument, collectKind(schema, {}), childPath,
                       schema.getTitle().value_or(property->getName()));
      names.push_back(builder.getStringAttr(property->getName()));
      verdicts.push_back(verdict);
    }
  }

  SmallVector<Attribute> requiredNames;
  if (const auto *required = node.find<RequiredKeyword>()) {
    for (const Located<std::string> &name : required->names) {
      if (declared.contains(name.value)) {
        requiredNames.push_back(builder.getStringAttr(name.value));
        continue;
      }
      InFlightDiagnostic diag =
          error(name.range.begin, "required property '" + name.value +
                                      "' is not declared in 'properties'");
      if (properties)
        diag.attachNote(loc(*properties)) << "'properties' is here";
    }
  }

  StringRef structName = node.getTitle().value_or(typeName);
  return createOp<StructOp>(
      builder, loc(anchor), builder.getI1Type(), input, verdicts,
      builder.getStringAttr(structName), builder.getArrayAttr(names),
      requiredNames.empty() ? ArrayAttr() : builder.getArrayAttr(requiredNames),
      /*additional_properties=*/UnitAttr());
}

OwningOpRef<ModuleOp> Importer::run(const SchemaNode &root) {
  context->loadDialect<SchemaDialect, arith::ArithDialect, func::FuncDialect>();

  StringRef stem = llvm::sys::path::filename(file.getName());
  stem = stem.take_until([](char c) { return c == '.'; });
  StringRef name = root.getTitle().value_or(stem);
  if (name.empty())
    name = "schema";

  Location rootLoc = loc(root);
  OwningOpRef<ModuleOp> module = ModuleOp::create(rootLoc);
  Type i1 = builder.getI1Type();
  auto function = func::FuncOp::create(
      rootLoc, getFunctionName(name),
      builder.getFunctionType({ValueType::get(context)}, {i1}));
  module->push_back(function);
  body = function.addEntryBlock();
  body->getArgument(0).setLoc(rootLoc);
  arguments[""] = body->getArgument(0);

  builder.setInsertionPointToEnd(body);
  Value verdict =
      importSchema(root, body->getArgument(0), collectKind(root, {}), "", name);
  createOp<func::ReturnOp>(builder, rootLoc, verdict);
  function.setFunctionType(
      builder.getFunctionType(body->getArgumentTypes(), {i1}));

  if (failed || mlir::failed(verify(*module)))
    return nullptr;
  return module;
}

OwningOpRef<ModuleOp> mlir::schema::importSchema(const SchemaNode &root,
                                                 const SourceFile &file) {
  return Importer(file).run(root);
}
