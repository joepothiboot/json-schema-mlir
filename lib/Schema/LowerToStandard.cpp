//===- LowerToStandard.cpp - schema -> arith/scf/math/func ---------------===//
//
// Implements `--lower-schema-to-std`.
//
// Lowering contract
// -----------------
//   !schema.value                 ->  i64                (opaque RT handle)
//   schema.validate_number        ->  scf.if + arith.cmpf/cmpi + math.trunc
//   schema.validate_string        ->  scf.if + arith.cmpi + func.call
//   schema.struct                 ->  scf.if + arith.andi reduction
//
// Every constraint is evaluated inside an `scf.if` guarded by the JSON type
// tag, because a typed assertion applied to a value of the wrong JSON type
// must evaluate to `false` rather than executing an ill-typed projection.
//
//===----------------------------------------------------------------------===//

#include "Schema/SchemaPasses.h"

#include "Schema/SchemaDialect.h"
#include "Schema/SchemaOps.h"
#include "Schema/SchemaTypes.h"

#include "mlir/Conversion/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SCF/Transforms/Patterns.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

#include <cstdint>
#include <optional>
#include <utility>

#define DEBUG_TYPE "lower-schema-to-std"

using namespace mlir;
using namespace mlir::schema;

//===----------------------------------------------------------------------===//
// StringPool
//===----------------------------------------------------------------------===//

int64_t StringPool::intern(StringAttr literal) {
  auto [it, inserted] =
      entries.insert({literal, static_cast<int64_t>(entries.size())});
  return it->second;
}

ArrayAttr StringPool::getTableAttr(MLIRContext *context) const {
  SmallVector<Attribute> table;
  table.reserve(entries.size());
  for (const auto &entry : entries)
    table.push_back(entry.first);
  return ArrayAttr::get(context, table);
}

//===----------------------------------------------------------------------===//
// SchemaTypeConverter
//===----------------------------------------------------------------------===//

SchemaTypeConverter::SchemaTypeConverter(MLIRContext *context) {
  // Identity for everything the standard dialects already understand.
  addConversion([](Type type) -> std::optional<Type> {
    if (isa<ValueType>(type))
      return std::nullopt;
    return type;
  });

  // The opaque JSON handle becomes an opaque machine word.
  addConversion([context](ValueType /*type*/) -> Type {
    return IntegerType::get(context, 64);
  });

  // Bridges for partially converted regions; the greedy folder in
  // `reconcile-unrealized-casts` removes any that survive.
  auto materializeCast = [](OpBuilder &builder, Type resultType,
                            ValueRange inputs, Location loc) -> Value {
    if (inputs.size() != 1)
      return Value();
    return builder.create<UnrealizedConversionCastOp>(loc, resultType, inputs)
        .getResult(0);
  };
  addSourceMaterialization(materializeCast);
  addTargetMaterialization(materializeCast);
}

//===----------------------------------------------------------------------===//
// Emission helpers
//===----------------------------------------------------------------------===//

namespace {

Value constF64(OpBuilder &b, Location loc, double value) {
  return b.create<arith::ConstantOp>(loc, b.getF64FloatAttr(value));
}

Value constI32(OpBuilder &b, Location loc, int32_t value) {
  return b.create<arith::ConstantOp>(loc, b.getI32IntegerAttr(value));
}

Value constI64(OpBuilder &b, Location loc, int64_t value) {
  return b.create<arith::ConstantOp>(loc, b.getI64IntegerAttr(value));
}

Value constBool(OpBuilder &b, Location loc, bool value) {
  return b.create<arith::ConstantOp>(loc, b.getBoolAttr(value));
}

/// `func.call @callee(args...) : (...) -> resultType`, referring to the
/// private declaration materialized by the pass before conversion begins.
Value emitRuntimeCall(OpBuilder &b, Location loc, StringRef callee,
                      Type resultType, ValueRange args) {
  return b.create<func::CallOp>(loc, callee, TypeRange{resultType}, args)
      .getResult(0);
}

/// `accumulator = accumulator && next`, short-circuit-free (all predicates are
/// pure and cheap once the type guard has been taken).
Value conjoin(OpBuilder &b, Location loc, Value accumulator, Value next) {
  if (!accumulator)
    return next;
  return b.create<arith::AndIOp>(loc, accumulator, next);
}

Value emitKindCheck(OpBuilder &b, Location loc, Value handle, JsonKind kind) {
  Value tag = emitRuntimeCall(b, loc, runtime::kKind, b.getI32Type(), handle);
  return b.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq, tag,
                                 constI32(b, loc, static_cast<int32_t>(kind)));
}

/// Builds `scf.if %guard -> (i1) { <body> } else { false }` and returns its
/// result. `bodyBuilder` returns the verdict yielded on the taken path.
Value emitGuardedVerdict(
    ConversionPatternRewriter &rewriter, Location loc, Value guard,
    llvm::function_ref<Value(ConversionPatternRewriter &, Location)>
        bodyBuilder) {
  Type i1Type = rewriter.getI1Type();

  // With a non-empty result-type list this builder creates both blocks but no
  // terminators, so the yields below are ours to place.
  auto ifOp = rewriter.create<scf::IfOp>(loc, TypeRange{i1Type}, guard,
                                         /*withElseRegion=*/true);

  rewriter.setInsertionPointToEnd(ifOp.thenBlock());
  rewriter.create<scf::YieldOp>(loc, bodyBuilder(rewriter, loc));

  rewriter.setInsertionPointToEnd(ifOp.elseBlock());
  rewriter.create<scf::YieldOp>(loc, constBool(rewriter, loc, false));

  rewriter.setInsertionPointAfter(ifOp);
  return ifOp.getResult(0);
}

//===----------------------------------------------------------------------===//
// schema.validate_number
//===----------------------------------------------------------------------===//

struct ValidateNumberLowering final : OpConversionPattern<ValidateNumberOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ValidateNumberOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    Value handle = adaptor.getInput();

    Value isNumber = emitKindCheck(rewriter, loc, handle, JsonKind::Number);

    Value verdict = emitGuardedVerdict(
        rewriter, loc, isNumber,
        [&](ConversionPatternRewriter &b, Location l) -> Value {
          Value x =
              emitRuntimeCall(b, l, runtime::kAsF64, b.getF64Type(), handle);
          Value accumulator;

          // minimum / exclusiveMinimum
          if (FloatAttr attr = op.getMinimumAttr()) {
            auto predicate = op.getExclusiveMinimum() ? arith::CmpFPredicate::OGT
                                                      : arith::CmpFPredicate::OGE;
            Value bound = constF64(b, l, attr.getValueAsDouble());
            accumulator = conjoin(
                b, l, accumulator,
                b.create<arith::CmpFOp>(l, predicate, x, bound));
          }

          // maximum / exclusiveMaximum
          if (FloatAttr attr = op.getMaximumAttr()) {
            auto predicate = op.getExclusiveMaximum() ? arith::CmpFPredicate::OLT
                                                      : arith::CmpFPredicate::OLE;
            Value bound = constF64(b, l, attr.getValueAsDouble());
            accumulator = conjoin(
                b, l, accumulator,
                b.create<arith::CmpFOp>(l, predicate, x, bound));
          }

          // multipleOf: `x / m` must be an exact integer.
          if (FloatAttr attr = op.getMultipleOfAttr()) {
            Value divisor = constF64(b, l, attr.getValueAsDouble());
            Value quotient = b.create<arith::DivFOp>(l, x, divisor);
            Value rounded = b.create<math::RoundEvenOp>(l, quotient);
            accumulator = conjoin(b, l, accumulator,
                                  b.create<arith::CmpFOp>(
                                      l, arith::CmpFPredicate::OEQ, quotient,
                                      rounded));
          }

          // integral: `x == trunc(x)`.
          if (op.getIntegral()) {
            Value truncated = b.create<math::TruncOp>(l, x);
            accumulator = conjoin(b, l, accumulator,
                                  b.create<arith::CmpFOp>(
                                      l, arith::CmpFPredicate::OEQ, x,
                                      truncated));
          }

          // An unconstrained `validate_number` degenerates to a type check.
          return accumulator ? accumulator : constBool(b, l, true);
        });

    rewriter.replaceOp(op, verdict);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// schema.validate_string
//===----------------------------------------------------------------------===//

struct ValidateStringLowering final : OpConversionPattern<ValidateStringOp> {
  ValidateStringLowering(const TypeConverter &typeConverter,
                         MLIRContext *context, StringPool &pool)
      : OpConversionPattern(typeConverter, context), pool(pool) {}

  LogicalResult
  matchAndRewrite(ValidateStringOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    Value handle = adaptor.getInput();

    Value isString = emitKindCheck(rewriter, loc, handle, JsonKind::String);

    Value verdict = emitGuardedVerdict(
        rewriter, loc, isString,
        [&](ConversionPatternRewriter &b, Location l) -> Value {
          Value accumulator;

          const bool needsLength =
              op.getMinLengthAttr() || op.getMaxLengthAttr();
          Value length;
          if (needsLength)
            length =
                emitRuntimeCall(b, l, runtime::kStrLen, b.getI64Type(), handle);

          if (IntegerAttr attr = op.getMinLengthAttr()) {
            Value bound = constI64(b, l, attr.getInt());
            accumulator = conjoin(b, l, accumulator,
                                  b.create<arith::CmpIOp>(
                                      l, arith::CmpIPredicate::sge, length,
                                      bound));
          }

          if (IntegerAttr attr = op.getMaxLengthAttr()) {
            Value bound = constI64(b, l, attr.getInt());
            accumulator = conjoin(b, l, accumulator,
                                  b.create<arith::CmpIOp>(
                                      l, arith::CmpIPredicate::sle, length,
                                      bound));
          }

          if (StringAttr attr = op.getPatternAttr()) {
            Value id = constI64(b, l, pool.intern(attr));
            accumulator = conjoin(b, l, accumulator,
                                  emitRuntimeCall(b, l, runtime::kStrMatches,
                                                  b.getI1Type(),
                                                  ValueRange{handle, id}));
          }

          if (StringAttr attr = op.getFormatAttr()) {
            Value id = constI64(b, l, pool.intern(attr));
            accumulator = conjoin(b, l, accumulator,
                                  emitRuntimeCall(b, l, runtime::kStrFormat,
                                                  b.getI1Type(),
                                                  ValueRange{handle, id}));
          }

          return accumulator ? accumulator : constBool(b, l, true);
        });

    rewriter.replaceOp(op, verdict);
    return success();
  }

private:
  StringPool &pool;
};

//===----------------------------------------------------------------------===//
// schema.struct
//===----------------------------------------------------------------------===//

struct StructLowering final : OpConversionPattern<StructOp> {
  StructLowering(const TypeConverter &typeConverter, MLIRContext *context,
                 StringPool &pool)
      : OpConversionPattern(typeConverter, context), pool(pool) {}

  LogicalResult
  matchAndRewrite(StructOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op.getLoc();
    Value handle = adaptor.getInput();

    Value isObject = emitKindCheck(rewriter, loc, handle, JsonKind::Object);

    // The per-field verdicts were produced by already-lowered validators and
    // are pure, so they may be computed outside the type guard.
    ValueRange fieldResults = adaptor.getFieldResults();
    ArrayAttr requiredFields = op.getRequiredFieldsAttr();

    Value verdict = emitGuardedVerdict(
        rewriter, loc, isObject,
        [&](ConversionPatternRewriter &b, Location l) -> Value {
          Value accumulator;

          if (requiredFields) {
            for (Attribute attr : requiredFields) {
              Value id = constI64(b, l, pool.intern(cast<StringAttr>(attr)));
              accumulator = conjoin(b, l, accumulator,
                                    emitRuntimeCall(b, l, runtime::kHasField,
                                                    b.getI1Type(),
                                                    ValueRange{handle, id}));
            }
          }

          for (Value fieldResult : fieldResults)
            accumulator = conjoin(b, l, accumulator, fieldResult);

          return accumulator ? accumulator : constBool(b, l, true);
        });

    rewriter.replaceOp(op, verdict);
    return success();
  }

private:
  StringPool &pool;
};

//===----------------------------------------------------------------------===//
// The pass
//===----------------------------------------------------------------------===//

struct LowerSchemaToStandardPass final
    : PassWrapper<LowerSchemaToStandardPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerSchemaToStandardPass)

  StringRef getArgument() const final { return "lower-schema-to-std"; }

  StringRef getDescription() const final {
    return "Lower the `schema` dialect to arith/scf/math/func";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, func::FuncDialect, math::MathDialect,
                    scf::SCFDialect>();
  }

  void runOnOperation() override {
    ModuleOp module = getOperation();
    MLIRContext *context = &getContext();

    // Runtime declarations are materialized up front: mutating the module's
    // symbol table from inside a conversion pattern is not legal, because the
    // rewriter only tracks changes rooted at the matched operation.
    declareRuntimeSymbols(module);

    SchemaTypeConverter typeConverter(context);
    StringPool pool;

    RewritePatternSet patterns(context);
    populateSchemaToStandardConversionPatternsImpl(typeConverter, pool,
                                                   patterns);

    ConversionTarget target(*context);
    configureSchemaToStandardTarget(typeConverter, target);

    if (failed(applyFullConversion(module, target, std::move(patterns)))) {
      module.emitError("failed to lower the `schema` dialect to standard "
                       "dialects");
      return signalPassFailure();
    }

    if (!pool.empty())
      module->setAttr("schema.string_pool", pool.getTableAttr(context));
  }

private:
  /// Emits, at the top of the module, the private declarations of the
  /// `libschema_runtime` entry points that the lowered IR calls.
  static void declareRuntimeSymbols(ModuleOp module) {
    OpBuilder builder(module.getBodyRegion());
    builder.setInsertionPointToStart(module.getBody());

    MLIRContext *context = module.getContext();
    Type i1 = builder.getI1Type();
    Type i32 = builder.getI32Type();
    Type i64 = builder.getI64Type();
    Type f64 = builder.getF64Type();

    const std::pair<StringRef, FunctionType> declarations[] = {
        {runtime::kKind, FunctionType::get(context, {i64}, {i32})},
        {runtime::kAsF64, FunctionType::get(context, {i64}, {f64})},
        {runtime::kStrLen, FunctionType::get(context, {i64}, {i64})},
        {runtime::kStrMatches, FunctionType::get(context, {i64, i64}, {i1})},
        {runtime::kStrFormat, FunctionType::get(context, {i64, i64}, {i1})},
        {runtime::kHasField, FunctionType::get(context, {i64, i64}, {i1})},
    };

    for (const auto &[name, signature] : declarations) {
      if (module.lookupSymbol<func::FuncOp>(name))
        continue;
      auto declaration =
          builder.create<func::FuncOp>(module.getLoc(), name, signature);
      declaration.setPrivate();
      // Purely functional projections of an immutable document.
      declaration->setAttr("llvm.readnone", UnitAttr::get(context));
    }
  }

  static void populateSchemaToStandardConversionPatternsImpl(
      const SchemaTypeConverter &typeConverter, StringPool &pool,
      RewritePatternSet &patterns) {
    populateSchemaToStandardConversionPatterns(typeConverter, pool, patterns);
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

void mlir::schema::populateSchemaToStandardConversionPatterns(
    const SchemaTypeConverter &typeConverter, StringPool &pool,
    RewritePatternSet &patterns) {
  MLIRContext *context = patterns.getContext();

  // Operation lowerings.
  patterns.add<ValidateNumberLowering>(typeConverter, context);
  patterns.add<ValidateStringLowering, StructLowering>(typeConverter, context,
                                                       pool);

  // Structural conversions: function signatures, calls, returns, branches and
  // the SCF region-carrying ops.
  populateFunctionOpInterfaceTypeConversionPattern<func::FuncOp>(patterns,
                                                                 typeConverter);
  populateCallOpTypeConversionPattern(patterns, typeConverter);
  populateReturnOpTypeConversionPattern(patterns, typeConverter);
  populateBranchOpInterfaceTypeConversionPattern(patterns, typeConverter);
  scf::populateSCFStructuralTypeConversions(typeConverter, patterns);
}

void mlir::schema::configureSchemaToStandardTarget(
    const SchemaTypeConverter &typeConverter, ConversionTarget &target) {
  target.addLegalDialect<arith::ArithDialect, math::MathDialect,
                         scf::SCFDialect>();
  target.addLegalOp<ModuleOp, UnrealizedConversionCastOp>();

  // The `schema` dialect must disappear entirely: a partial conversion would
  // leave a validation op holding an `i64` where its ODS constraint demands
  // `!schema.value`, which fails verification.
  target.addIllegalDialect<SchemaDialect>();

  target.addDynamicallyLegalOp<func::FuncOp>([&](func::FuncOp op) {
    return typeConverter.isSignatureLegal(op.getFunctionType()) &&
           typeConverter.isLegal(&op.getBody());
  });
  target.addDynamicallyLegalOp<func::CallOp>(
      [&](func::CallOp op) { return typeConverter.isLegal(op); });
  target.addDynamicallyLegalOp<func::ReturnOp>(
      [&](func::ReturnOp op) { return typeConverter.isLegal(op); });
  target.markUnknownOpDynamicallyLegal([&](Operation *op) {
    return isNotBranchOpInterfaceOrReturnLikeOp(op) ||
           isLegalForBranchOpInterfaceTypeConversionPattern(op,
                                                            typeConverter) ||
           isLegalForReturnOpTypeConversionPattern(op, typeConverter);
  });
}

std::unique_ptr<Pass> mlir::schema::createLowerSchemaToStandardPass() {
  return std::make_unique<LowerSchemaToStandardPass>();
}

void mlir::schema::buildSchemaToStandardPipeline(OpPassManager &pm) {
  pm.addPass(createCanonicalizerPass());
  pm.addNestedPass<func::FuncOp>(createSchemaCanonicalizerPass());
  pm.addPass(createLowerSchemaToStandardPass());
  pm.addPass(createReconcileUnrealizedCastsPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  pm.addPass(createSymbolDCEPass());
}

void mlir::schema::registerSchemaLoweringPasses() {
  PassRegistration<LowerSchemaToStandardPass>();

  PassPipelineRegistration<>(
      "schema-to-std-pipeline",
      "Canonicalize the `schema` dialect and lower it to arith/scf/math/func",
      buildSchemaToStandardPipeline);

  PassPipelineRegistration<>(
      "schema-to-llvm-pipeline",
      "Lower the `schema` dialect all the way to the LLVM dialect",
      [](OpPassManager &pm) {
        buildSchemaToStandardPipeline(pm);
        pm.addPass(createSCFToControlFlowPass());
        pm.addPass(createConvertToLLVMPass());
        pm.addPass(createReconcileUnrealizedCastsPass());
      });
}