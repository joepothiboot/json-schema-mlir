//===- Trace.cpp - Front-end trace for external viewers -------------------===//
//
// Ops are tied to AST nodes through their locations: the importer gives each
// op the `FileLineColLoc` of the node's first byte, and no two nodes start at
// the same position. The mapping therefore survives passes that fuse
// locations (`--schema-canonicalize` builds a `FusedLoc` of every merged op).
//
//===----------------------------------------------------------------------===//

#include "Schema/Import/Trace.h"

#include "mlir/IR/AsmState.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <utility>

using namespace mlir;
using namespace mlir::schema;
using namespace mlir::schema::json;

TraceDiagnostic TraceDiagnostic::from(Diagnostic &diag) {
  TraceDiagnostic result{"", diag.str(), diag.getLocation(), {}};
  switch (diag.getSeverity()) {
  case DiagnosticSeverity::Note:
    result.severity = "note";
    break;
  case DiagnosticSeverity::Warning:
    result.severity = "warning";
    break;
  case DiagnosticSeverity::Error:
    result.severity = "error";
    break;
  case DiagnosticSeverity::Remark:
    result.severity = "remark";
    break;
  }
  for (Diagnostic &note : diag.getNotes())
    result.notes.push_back(from(note));
  return result;
}

namespace {

using LineCol = std::pair<unsigned, unsigned>;

LineCol key(SourcePos pos) { return {pos.line, pos.column}; }

StringRef getKeywordCategory(const KeywordNode &keyword) {
  return llvm::TypeSwitch<const KeywordNode *, StringRef>(&keyword)
      .Case([](const TypeKeyword *) { return "type"; })
      .Case([](const PropertiesKeyword *) { return "properties"; })
      .Case([](const RequiredKeyword *) { return "required"; })
      .Case([](const AllOfKeyword *) { return "allOf"; })
      .Case([](const NumberKeyword *) { return "number"; })
      .Case([](const LengthKeyword *) { return "length"; })
      .Case([](const StringKeyword *) { return "string"; })
      .Case([](const AnnotationKeyword *) { return "annotation"; })
      .Default([](const KeywordNode *) { return "unsupported"; });
}

class TraceWriter {
public:
  TraceWriter(llvm::raw_ostream &os, const SourceFile &file,
              ArrayRef<Token> tokens, const SchemaNode *root)
      : j(os, /*IndentSize=*/2), file(file), tokens(tokens), root(root) {
    for (const Token &token : tokens)
      tokenAt.try_emplace(key(token.range.begin), &token);
    if (root)
      walk(*root, [&](const Node &node) {
        nodeAt.try_emplace(key(node.getRange().begin), &node);
      });
  }

  void write(ArrayRef<TraceDiagnostic> diagnostics,
             ArrayRef<TraceStage> stages) {
    j.object([&] {
      j.attribute("format", "json-schema-mlir-trace");
      j.attribute("version", kTraceFormatVersion);
      j.attributeObject("source", [&] {
        j.attribute("file", file.getName());
        j.attribute("text", file.getText());
      });
      j.attributeArray("tokens", [&] {
        for (const Token &token : tokens)
          writeToken(token);
      });
      j.attributeBegin("ast");
      if (root)
        writeNode(*root);
      else
        j.value(nullptr);
      j.attributeEnd();
      j.attributeArray("diagnostics", [&] {
        for (const TraceDiagnostic &diag : diagnostics)
          writeDiagnostic(diag);
      });
      j.attributeArray("stages", [&] {
        for (const TraceStage &stage : stages)
          writeStage(stage);
      });
    });
  }

private:
  //===--------------------------------------------------------------------===//
  // Positions
  //===--------------------------------------------------------------------===//

  void writePos(StringRef name, SourcePos pos) {
    j.attributeObject(name, [&] {
      j.attribute("offset", pos.offset);
      j.attribute("line", pos.line);
      j.attribute("column", pos.column);
    });
  }

  void writeRange(StringRef name, const SourceRange &range) {
    j.attributeObject(name, [&] {
      writePos("begin", range.begin);
      writePos("end", range.end);
    });
  }

  /// The file:line:col locations inside `loc` that point into this file.
  SmallVector<LineCol> getPositions(Location loc) {
    llvm::SetVector<LineCol> positions;
    loc->walk([&](Location nested) {
      if (auto fileLoc = llvm::dyn_cast<FileLineColLoc>(nested))
        if (fileLoc.getFilename() == file.getName())
          positions.insert({fileLoc.getLine(), fileLoc.getColumn()});
      return WalkResult::advance();
    });
    return positions.takeVector();
  }

  /// A located position widened to the token that starts there, if any.
  SourceRange getRange(LineCol position) {
    if (const Token *token = tokenAt.lookup(position))
      return token->range;
    SourcePos pos{file.getOffset(position.first, position.second),
                  position.first, position.second};
    return {pos, pos};
  }

  //===--------------------------------------------------------------------===//
  // Tokens and AST
  //===--------------------------------------------------------------------===//

  void writeToken(const Token &token) {
    j.object([&] {
      j.attribute("kind", stringifyTokenKind(token.kind));
      j.attribute("text", token.spelling);
      writeRange("range", token.range);
    });
  }

  void writeStrings(StringRef name, ArrayRef<Located<std::string>> strings) {
    j.attributeArray(name, [&] {
      for (const Located<std::string> &string : strings)
        j.value(string.value);
    });
  }

  void writeNode(const Node &node) {
    j.object([&] {
      j.attribute("id", node.getId());
      llvm::TypeSwitch<const Node *>(&node)
          .Case([&](const SchemaNode *) { j.attribute("kind", "schema"); })
          .Case([&](const PropertyNode *property) {
            j.attribute("kind", "property");
            j.attribute("name", property->getName());
            writeRange("nameRange", property->getNameRange());
          })
          .Case([&](const KeywordNode *keyword) {
            j.attribute("kind", "keyword");
            j.attribute("keyword", keyword->getName());
            j.attribute("category", getKeywordCategory(*keyword));
            writeRange("keyRange", keyword->getKeyRange());
          });
      writeRange("range", node.getRange());
      writeValue(node);
      j.attributeArray("children", [&] {
        for (const Node *child : getChildren(node))
          writeNode(*child);
      });
    });
  }

  void writeValue(const Node &node) {
    llvm::TypeSwitch<const Node *>(&node)
        .Case([&](const TypeKeyword *type) {
          j.attributeArray("value", [&] {
            for (const Located<JsonType> &entry : type->types)
              j.value(stringifyJsonType(entry.value));
          });
        })
        .Case([&](const RequiredKeyword *required) {
          writeStrings("value", required->names);
        })
        .Case([&](const NumberKeyword *number) {
          j.attribute("value", number->value);
          j.attribute("spelling", number->spelling);
        })
        .Case([&](const LengthKeyword *length) {
          j.attribute("value", length->value);
        })
        .Case([&](const StringKeyword *string) {
          j.attribute("value", string->value);
        })
        .Case([&](const AnnotationKeyword *annotation) {
          if (annotation->text)
            j.attribute("value", *annotation->text);
        });
  }

  //===--------------------------------------------------------------------===//
  // Diagnostics
  //===--------------------------------------------------------------------===//

  void writeDiagnostic(const TraceDiagnostic &diag) {
    j.object([&] {
      j.attribute("severity", diag.severity);
      j.attribute("message", diag.message);
      SmallVector<LineCol> positions = getPositions(diag.location);
      j.attributeBegin("range");
      if (positions.empty()) {
        j.value(nullptr);
      } else {
        SourceRange range = getRange(positions.front());
        j.object([&] {
          writePos("begin", range.begin);
          writePos("end", range.end);
        });
      }
      j.attributeEnd();
      j.attributeArray("notes", [&] {
        for (const TraceDiagnostic &note : diag.notes)
          writeDiagnostic(note);
      });
    });
  }

  //===--------------------------------------------------------------------===//
  // IR
  //===--------------------------------------------------------------------===//

  void writeStage(const TraceStage &stage) {
    std::string text;
    llvm::raw_string_ostream os(text);
    AsmState::LocationMap printed;
    AsmState state(stage.module, OpPrintingFlags(), &printed);
    stage.module->print(os, state);

    llvm::DenseMap<Operation *, unsigned> ids;
    stage.module->walk<WalkOrder::PreOrder>(
        [&](Operation *op) { ids.try_emplace(op, ids.size()); });

    j.object([&] {
      j.attribute("name", stage.name);
      j.attribute("ir", text);
      j.attributeArray("ops", [&] {
        stage.module->walk<WalkOrder::PreOrder>(
            [&](Operation *op) { writeOp(op, ids, printed); });
      });
    });
  }

  void writeOp(Operation *op, const llvm::DenseMap<Operation *, unsigned> &ids,
               const AsmState::LocationMap &printed) {
    SmallVector<LineCol> positions = getPositions(op->getLoc());
    SmallVector<const Node *> nodes;
    for (LineCol position : positions)
      if (const Node *node = nodeAt.lookup(position))
        nodes.push_back(node);

    j.object([&] {
      j.attribute("id", ids.lookup(op));
      j.attribute("name", op->getName().getStringRef());
      j.attributeBegin("parent");
      if (Operation *parent = op->getParentOp())
        j.value(ids.lookup(parent));
      else
        j.value(nullptr);
      j.attributeEnd();

      // AsmState reports 1-based lines but 0-based columns; the trace uses
      // 1-based columns throughout.
      j.attributeBegin("irPos");
      if (auto it = printed.find(op); it != printed.end())
        j.object([&] {
          j.attribute("line", it->second.first);
          j.attribute("column", it->second.second + 1);
        });
      else
        j.value(nullptr);
      j.attributeEnd();

      j.attributeArray("astNodes", [&] {
        for (const Node *node : nodes)
          j.value(node->getId());
      });
      j.attributeArray("ranges", [&] {
        for (const Node *node : nodes)
          j.object([&] {
            writePos("begin", node->getRange().begin);
            writePos("end", node->getRange().end);
          });
      });
    });
  }

  llvm::json::OStream j;
  const SourceFile &file;
  ArrayRef<Token> tokens;
  const SchemaNode *root;
  llvm::DenseMap<LineCol, const Token *> tokenAt;
  llvm::DenseMap<LineCol, const Node *> nodeAt;
};

} // namespace

void mlir::schema::writeTrace(llvm::raw_ostream &os, const SourceFile &file,
                              ArrayRef<Token> tokens, const SchemaNode *root,
                              ArrayRef<TraceDiagnostic> diagnostics,
                              ArrayRef<TraceStage> stages) {
  TraceWriter(os, file, tokens, root).write(diagnostics, stages);
  os << '\n';
}
