//===- SchemaPasses.h - Transformation & conversion passes ---------------===//
#ifndef SCHEMA_SCHEMAPASSES_H
#define SCHEMA_SCHEMAPASSES_H

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>

namespace mlir {
class RewritePatternSet;
class OpPassManager;
} // namespace mlir

namespace mlir::schema {

//===----------------------------------------------------------------------===//
// Runtime ABI
//===----------------------------------------------------------------------===//

/// Symbol names of the `libschema_runtime` shim that the lowered IR calls into.
/// Every entry point is `readonly` and takes an opaque `i64` document handle.
namespace runtime {
inline constexpr ::llvm::StringLiteral kKind = "__schema_rt_kind";
inline constexpr ::llvm::StringLiteral kAsF64 = "__schema_rt_as_f64";
inline constexpr ::llvm::StringLiteral kStrLen = "__schema_rt_str_len";
inline constexpr ::llvm::StringLiteral kStrMatches = "__schema_rt_str_matches";
inline constexpr ::llvm::StringLiteral kStrFormat = "__schema_rt_str_format";
inline constexpr ::llvm::StringLiteral kHasField = "__schema_rt_has_field";
} // namespace runtime

/// JSON type tags returned by `__schema_rt_kind`. Must stay in sync with the
/// runtime shim's `schema_kind_t` enum.
enum class JsonKind : int32_t {
  Null = 0,
  Boolean = 1,
  Number = 2,
  String = 3,
  Array = 4,
  Object = 5,
};

//===----------------------------------------------------------------------===//
// String interning
//===----------------------------------------------------------------------===//

/// Interns the regex / format / field-name literals encountered during
/// lowering. The resulting table is attached to the module as the
/// `schema.string_pool` array attribute, and the lowered IR refers to entries
/// by their `i64` index. This keeps the conversion free of LLVM globals so it
/// can run before `--convert-to-llvm`.
class StringPool {
public:
  int64_t intern(::mlir::StringAttr literal);
  ::mlir::ArrayAttr getTableAttr(::mlir::MLIRContext *context) const;
  bool empty() const { return entries.empty(); }

private:
  ::llvm::MapVector<::mlir::StringAttr, int64_t> entries;
};

//===----------------------------------------------------------------------===//
// Type conversion
//===----------------------------------------------------------------------===//

/// Converts `!schema.value` to an opaque `i64` runtime document handle and
/// leaves every builtin type untouched.
class SchemaTypeConverter : public ::mlir::TypeConverter {
public:
  SchemaTypeConverter(::mlir::MLIRContext *context);
};

//===----------------------------------------------------------------------===//
// Pattern population
//===----------------------------------------------------------------------===//

/// Populates the `schema` -> `arith`/`scf`/`math`/`func` conversion patterns.
void populateSchemaToStandardConversionPatterns(
    const SchemaTypeConverter &typeConverter, StringPool &pool,
    ::mlir::RewritePatternSet &patterns);

/// Marks `schema` illegal and `arith`/`scf`/`math`/`func` legal, including the
/// signature legality of function-like and call-like operations.
void configureSchemaToStandardTarget(const SchemaTypeConverter &typeConverter,
                                     ::mlir::ConversionTarget &target);

//===----------------------------------------------------------------------===//
// Canonicalization (from the previous phase)
//===----------------------------------------------------------------------===//

void populateSchemaCanonicalizationPatterns(::mlir::RewritePatternSet &patterns);
void populateSchemaConstraintNormalizationPatterns(
    ::mlir::RewritePatternSet &patterns);
std::unique_ptr<::mlir::Pass> createSchemaCanonicalizerPass();

//===----------------------------------------------------------------------===//
// Pass & pipeline construction
//===----------------------------------------------------------------------===//

/// Creates the `--lower-schema-to-std` pass.
std::unique_ptr<::mlir::Pass> createLowerSchemaToStandardPass();

/// Canonicalize -> lower -> clean up. Leaves `arith`/`scf`/`math`/`func` IR
/// that `--convert-to-llvm` consumes verbatim.
void buildSchemaToStandardPipeline(::mlir::OpPassManager &pm);

/// Registers all `schema` passes and pipelines with the global registry.
void registerSchemaPasses();

/// Registers lowering passes and the standard/LLVM pipelines. Kept separate
/// because the lowering pass implementation is private to its translation
/// unit.
void registerSchemaLoweringPasses();

} // namespace mlir::schema

#endif // SCHEMA_SCHEMAPASSES_H