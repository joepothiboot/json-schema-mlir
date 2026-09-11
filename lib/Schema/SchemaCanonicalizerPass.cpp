//===- SchemaCanonicalizerPass.cpp - Redundant validation elimination -----===//
//
// Implements `--schema-canonicalize`.
//
// The transformation is built on a small constraint lattice per validation
// flavour. For two constraint sets C1, C2:
//
//   * `subsumes(C1, C2)` holds iff every JSON value accepted by C1 is also
//     accepted by C2 (C1 is at least as strict).
//   * `meet(C1, C2)` is the greatest lower bound, i.e. the single constraint
//     set equivalent to `C1 AND C2`. It is partial: two distinct regular
//     expressions have no representable meet, so the merge is refused.
//
// Because `C1 AND C2 == meet(C1, C2)`, an entire `arith.andi` tree over
// validators of the same SSA input collapses to one operation per input.
//
//===----------------------------------------------------------------------===//

#include "Schema/SchemaPasses.h"

#include "Schema/SchemaDialect.h"
#include "Schema/SchemaOps.h"
#include "Schema/SchemaTypes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#define DEBUG_TYPE "schema-canonicalize"

using namespace mlir;
using namespace mlir::schema;

//===----------------------------------------------------------------------===//
// LLVM/MLIR version compatibility shims
//===----------------------------------------------------------------------===//

namespace {
/// `applyPatternsAndFoldGreedily` was renamed to `applyPatternsGreedily` and
/// `GreedyRewriteConfig`'s fields became private (setter-based) in LLVM 20.
inline LogicalResult applyGreedily(Operation *op,
                                   const FrozenRewritePatternSet &patterns,
                                   int64_t maxIterations) {
  GreedyRewriteConfig config;
#if LLVM_VERSION_MAJOR >= 20
  config.setMaxIterations(maxIterations).setUseTopDownTraversal(true);
  return applyPatternsGreedily(op, patterns, config);
#else
  config.maxIterations = maxIterations;
  config.useTopDownTraversal = true;
  return applyPatternsAndFoldGreedily(op, patterns, config);
#endif
}
} // namespace

//===----------------------------------------------------------------------===//
// Constraint lattices
//===----------------------------------------------------------------------===//

namespace {

/// Tightest of two optional lower bounds (larger wins).
template <typename T>
std::optional<T> tightestLower(std::optional<T> a, std::optional<T> b) {
  if (!a)
    return b;
  if (!b)
    return a;
  return std::max(*a, *b);
}

/// Tightest of two optional upper bounds (smaller wins).
template <typename T>
std::optional<T> tightestUpper(std::optional<T> a, std::optional<T> b) {
  if (!a)
    return b;
  if (!b)
    return a;
  return std::min(*a, *b);
}

/// True when `a` is an exact integral multiple of `b`.
bool isIntegralMultipleOf(double a, double b) {
  if (b == 0.0 || !std::isfinite(a) || !std::isfinite(b))
    return false;
  const double quotient = a / b;
  return std::isfinite(quotient) && quotient != 0.0 &&
         quotient == std::trunc(quotient);
}

//===----------------------------------------------------------------------===//
// StringLattice
//===----------------------------------------------------------------------===//

struct StringLattice {
  std::optional<int64_t> minLength;
  std::optional<int64_t> maxLength;
  StringAttr pattern; // null == unconstrained
  StringAttr format;  // null == unconstrained

  static StringLattice from(ValidateStringOp op) {
    StringLattice lattice;
    if (IntegerAttr attr = op.getMinLengthAttr())
      lattice.minLength = attr.getInt();
    if (IntegerAttr attr = op.getMaxLengthAttr())
      lattice.maxLength = attr.getInt();
    lattice.pattern = op.getPatternAttr();
    lattice.format = op.getFormatAttr();
    return lattice;
  }

  bool operator==(const StringLattice &rhs) const {
    return minLength == rhs.minLength && maxLength == rhs.maxLength &&
           pattern == rhs.pattern && format == rhs.format;
  }

  /// `*this` accepts a subset of what `rhs` accepts.
  bool subsumes(const StringLattice &rhs) const {
    if (rhs.minLength && (!minLength || *minLength < *rhs.minLength))
      return false;
    if (rhs.maxLength && (!maxLength || *maxLength > *rhs.maxLength))
      return false;
    // Regular-expression containment is undecidable in general; require
    // syntactic identity.
    if (rhs.pattern && pattern != rhs.pattern)
      return false;
    if (rhs.format && format != rhs.format)
      return false;
    return true;
  }

  /// Greatest lower bound. `std::nullopt` when not representable as a single
  /// `schema.validate_string`.
  static std::optional<StringLattice> meet(const StringLattice &a,
                                           const StringLattice &b) {
    if (a.pattern && b.pattern && a.pattern != b.pattern)
      return std::nullopt;
    if (a.format && b.format && a.format != b.format)
      return std::nullopt;

    StringLattice result;
    result.minLength = tightestLower(a.minLength, b.minLength);
    result.maxLength = tightestUpper(a.maxLength, b.maxLength);
    result.pattern = a.pattern ? a.pattern : b.pattern;
    result.format = a.format ? a.format : b.format;
    return result;
  }
};

//===----------------------------------------------------------------------===//
// NumberLattice
//===----------------------------------------------------------------------===//

struct NumberLattice {
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::optional<double> multipleOf;
  bool exclusiveMin = false;
  bool exclusiveMax = false;
  bool integral = false;

  static NumberLattice from(ValidateNumberOp op) {
    NumberLattice lattice;
    if (FloatAttr attr = op.getMinimumAttr())
      lattice.minimum = attr.getValueAsDouble();
    if (FloatAttr attr = op.getMaximumAttr())
      lattice.maximum = attr.getValueAsDouble();
    if (FloatAttr attr = op.getMultipleOfAttr())
      lattice.multipleOf = attr.getValueAsDouble();
    lattice.exclusiveMin = op.getExclusiveMinimum();
    lattice.exclusiveMax = op.getExclusiveMaximum();
    lattice.integral = op.getIntegral();
    return lattice;
  }

  bool operator==(const NumberLattice &rhs) const {
    return minimum == rhs.minimum && maximum == rhs.maximum &&
           multipleOf == rhs.multipleOf && exclusiveMin == rhs.exclusiveMin &&
           exclusiveMax == rhs.exclusiveMax && integral == rhs.integral;
  }

  /// Is lower bound (a, aExcl) at least as strict as (b, bExcl)?
  static bool lowerAtLeastAsStrict(std::optional<double> a, bool aExcl,
                                   std::optional<double> b, bool bExcl) {
    if (!b)
      return true;
    if (!a)
      return false;
    if (*a > *b)
      return true;
    if (*a < *b)
      return false;
    return aExcl || !bExcl;
  }

  static bool upperAtLeastAsStrict(std::optional<double> a, bool aExcl,
                                   std::optional<double> b, bool bExcl) {
    if (!b)
      return true;
    if (!a)
      return false;
    if (*a < *b)
      return true;
    if (*a > *b)
      return false;
    return aExcl || !bExcl;
  }

  bool subsumes(const NumberLattice &rhs) const {
    if (!lowerAtLeastAsStrict(minimum, exclusiveMin, rhs.minimum,
                              rhs.exclusiveMin))
      return false;
    if (!upperAtLeastAsStrict(maximum, exclusiveMax, rhs.maximum,
                              rhs.exclusiveMax))
      return false;
    if (rhs.multipleOf &&
        (!multipleOf || !isIntegralMultipleOf(*multipleOf, *rhs.multipleOf)))
      return false;
    if (rhs.integral && !integral)
      return false;
    return true;
  }

  static std::optional<NumberLattice> meet(const NumberLattice &a,
                                           const NumberLattice &b) {
    NumberLattice result;

    if (lowerAtLeastAsStrict(a.minimum, a.exclusiveMin, b.minimum,
                             b.exclusiveMin)) {
      result.minimum = a.minimum;
      result.exclusiveMin = a.exclusiveMin;
    } else {
      result.minimum = b.minimum;
      result.exclusiveMin = b.exclusiveMin;
    }

    if (upperAtLeastAsStrict(a.maximum, a.exclusiveMax, b.maximum,
                             b.exclusiveMax)) {
      result.maximum = a.maximum;
      result.exclusiveMax = a.exclusiveMax;
    } else {
      result.maximum = b.maximum;
      result.exclusiveMax = b.exclusiveMax;
    }

    // `multipleOf` only merges when one divisor is a multiple of the other;
    // a real-valued LCM is not generally representable.
    if (a.multipleOf && b.multipleOf) {
      if (isIntegralMultipleOf(*a.multipleOf, *b.multipleOf))
        result.multipleOf = a.multipleOf;
      else if (isIntegralMultipleOf(*b.multipleOf, *a.multipleOf))
        result.multipleOf = b.multipleOf;
      else
        return std::nullopt;
    } else {
      result.multipleOf = a.multipleOf ? a.multipleOf : b.multipleOf;
    }

    result.integral = a.integral || b.integral;
    return result;
  }
};

//===----------------------------------------------------------------------===//
// Materialization helpers
//===----------------------------------------------------------------------===//

Value materialize(PatternRewriter &rewriter, Location loc, Value input,
                  const StringLattice &lattice) {
  return rewriter.create<ValidateStringOp>(
      loc, rewriter.getI1Type(), input,
      lattice.minLength ? rewriter.getI64IntegerAttr(*lattice.minLength)
                        : IntegerAttr(),
      lattice.maxLength ? rewriter.getI64IntegerAttr(*lattice.maxLength)
                        : IntegerAttr(),
      lattice.pattern, lattice.format);
}

Value materialize(PatternRewriter &rewriter, Location loc, Value input,
                  const NumberLattice &lattice) {
  UnitAttr unit = rewriter.getUnitAttr();
  return rewriter.create<ValidateNumberOp>(
      loc, rewriter.getI1Type(), input,
      lattice.minimum ? rewriter.getF64FloatAttr(*lattice.minimum)
                      : FloatAttr(),
      lattice.maximum ? rewriter.getF64FloatAttr(*lattice.maximum)
                      : FloatAttr(),
      lattice.multipleOf ? rewriter.getF64FloatAttr(*lattice.multipleOf)
                         : FloatAttr(),
      lattice.exclusiveMin ? unit : UnitAttr(),
      lattice.exclusiveMax ? unit : UnitAttr(),
      lattice.integral ? unit : UnitAttr());
}

//===----------------------------------------------------------------------===//
// Conjunct analysis
//===----------------------------------------------------------------------===//

/// One leaf of a boolean conjunction tree, annotated with its constraint
/// lattice when the leaf is a `schema` validation.
struct Conjunct {
  Value value;                        // the live i1 SSA value
  Value input;                        // validated !schema.value, null if opaque
  Location loc;                       // (fused) location of the merged result
  std::optional<StringLattice> str;
  std::optional<NumberLattice> num;
  bool dead = false;                  // absorbed into another conjunct
  bool dirty = false;                 // lattice diverged from `value`'s def op

  bool isValidator() const { return str.has_value() || num.has_value(); }
};

Conjunct classify(Value value) {
  Conjunct conjunct{value, {}, value.getLoc()};

  if (auto strOp = value.getDefiningOp<ValidateStringOp>()) {
    conjunct.input = strOp.getInput();
    conjunct.str = StringLattice::from(strOp);
  } else if (auto numOp = value.getDefiningOp<ValidateNumberOp>()) {
    conjunct.input = numOp.getInput();
    conjunct.num = NumberLattice::from(numOp);
  }
  return conjunct;
}

/// Flattens a maximal `arith.andi` tree of `i1` values into its leaves.
/// Interior nodes are only traversed when they are single-use, so that the
/// tree can be rebuilt without leaving observable dangling definitions.
void collectConjuncts(Value value, SmallVectorImpl<Value> &leaves) {
  if (auto andOp = value.getDefiningOp<arith::AndIOp>()) {
    if (andOp.getType().isInteger(1) && andOp->hasOneUse()) {
      collectConjuncts(andOp.getLhs(), leaves);
      collectConjuncts(andOp.getRhs(), leaves);
      return;
    }
  }
  leaves.push_back(value);
}

/// Left-associated reconstruction of the conjunction.
Value buildConjunction(PatternRewriter &rewriter, Location loc,
                       ArrayRef<Value> values) {
  assert(!values.empty() && "empty conjunction");
  Value accumulator = values.front();
  for (Value value : values.drop_front())
    accumulator = rewriter.create<arith::AndIOp>(loc, accumulator, value);
  return accumulator;
}

/// Absorbs `src` into `dst` when their constraints admit a meet.
/// Returns false when the two conjuncts are not combinable.
template <typename LatticeT>
bool absorb(Conjunct &dst, const Conjunct &src,
            std::optional<LatticeT> Conjunct::*field) {
  std::optional<LatticeT> merged =
      LatticeT::meet(*(dst.*field), *(src.*field));
  if (!merged)
    return false;

  if (*merged == *(dst.*field)) {
    // `dst` already expresses the merged constraint: keep its SSA value.
  } else if (*merged == *(src.*field)) {
    // `src` is strictly stronger: adopt its (already materialized) value.
    dst.value = src.value;
    dst.dirty = false;
  } else {
    // Genuine fusion: a fresh operation is required.
    dst.dirty = true;
  }
  dst.*field = merged;
  dst.loc = FusedLoc::get(dst.value.getContext(), {dst.loc, src.loc});
  return true;
}

//===----------------------------------------------------------------------===//
// Pattern: fold conjunctions of validations
//===----------------------------------------------------------------------===//

/// Rewrites a maximal `arith.andi` tree so that all validations applied to the
/// same `!schema.value` input collapse into a single operation carrying the
/// meet of their constraints.
///
///   %a = schema.validate_string %d {min_length = 5}
///   %b = schema.validate_string %d {min_length = 2}
///   %r = arith.andi %a, %b
///   ==>
///   %r = schema.validate_string %d {min_length = 5}    // %b becomes dead
struct FoldConjunctiveValidations final : OpRewritePattern<arith::AndIOp> {
  FoldConjunctiveValidations(MLIRContext *context, PatternBenefit benefit = 2)
      : OpRewritePattern(context, benefit) {}

  LogicalResult matchAndRewrite(arith::AndIOp root,
                                PatternRewriter &rewriter) const override {
    if (!root.getType().isInteger(1))
      return rewriter.notifyMatchFailure(root, "not a boolean conjunction");

    // Rewrite once, from the root of the tree, to keep the transformation
    // O(n) in the number of conjuncts and to guarantee termination.
    if (llvm::any_of(root->getUsers(),
                     [](Operation *user) { return isa<arith::AndIOp>(user); }))
      return rewriter.notifyMatchFailure(root, "interior `andi` node");

    SmallVector<Value> flattened;
    collectConjuncts(root.getLhs(), flattened);
    collectConjuncts(root.getRhs(), flattened);

    // `x && x == x`.
    SetVector<Value> unique(flattened.begin(), flattened.end());
    bool changed = unique.size() != flattened.size();

    SmallVector<Conjunct> conjuncts;
    conjuncts.reserve(unique.size());
    for (Value value : unique)
      conjuncts.push_back(classify(value));

    // Pairwise absorption. The conjunct count is tiny in practice, so the
    // quadratic sweep is cheaper than building hash maps.
    for (size_t i = 0, e = conjuncts.size(); i < e; ++i) {
      Conjunct &dst = conjuncts[i];
      if (dst.dead || !dst.isValidator())
        continue;
      for (size_t j = i + 1; j < e; ++j) {
        Conjunct &src = conjuncts[j];
        if (src.dead || src.input != dst.input)
          continue;
        bool absorbed = false;
        if (dst.str && src.str)
          absorbed = absorb<StringLattice>(dst, src, &Conjunct::str);
        else if (dst.num && src.num)
          absorbed = absorb<NumberLattice>(dst, src, &Conjunct::num);
        if (absorbed) {
          src.dead = true;
          changed = true;
        }
      }
    }

    if (!changed)
      return rewriter.notifyMatchFailure(root, "no redundant validation found");

    rewriter.setInsertionPoint(root);

    SmallVector<Value> operands;
    operands.reserve(conjuncts.size());
    for (Conjunct &conjunct : conjuncts) {
      if (conjunct.dead)
        continue;
      if (!conjunct.dirty) {
        operands.push_back(conjunct.value);
        continue;
      }
      if (conjunct.str)
        operands.push_back(
            materialize(rewriter, conjunct.loc, conjunct.input, *conjunct.str));
      else
        operands.push_back(
            materialize(rewriter, conjunct.loc, conjunct.input, *conjunct.num));
    }

    LLVM_DEBUG(llvm::dbgs() << "[schema] folded " << flattened.size() << " -> "
                            << operands.size() << " conjuncts\n");

    rewriter.replaceOp(root,
                       buildConjunction(rewriter, root.getLoc(), operands));
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern: drop subsumed validators from schema.struct
//===----------------------------------------------------------------------===//

/// `schema.struct` reduces its `field_results` conjunctively, so a validator
/// operand may be replaced by any strictly stronger validator operand applied
/// to the same input. The weakened definition then becomes trivially dead and
/// is removed by the greedy driver.
struct DropSubsumedStructValidators final : OpRewritePattern<StructOp> {
  using OpRewritePattern::OpRewritePattern;

  /// True when `strong` is *strictly* stronger than `weak`. Strictness makes
  /// the rewrite a well-founded descent, guaranteeing termination.
  static bool strictlyStronger(Value strong, Value weak) {
    if (strong == weak)
      return false;

    if (auto a = strong.getDefiningOp<ValidateStringOp>()) {
      auto b = weak.getDefiningOp<ValidateStringOp>();
      if (!b || a.getInput() != b.getInput())
        return false;
      StringLattice la = StringLattice::from(a);
      StringLattice lb = StringLattice::from(b);
      return la.subsumes(lb) && !lb.subsumes(la);
    }

    if (auto a = strong.getDefiningOp<ValidateNumberOp>()) {
      auto b = weak.getDefiningOp<ValidateNumberOp>();
      if (!b || a.getInput() != b.getInput())
        return false;
      NumberLattice la = NumberLattice::from(a);
      NumberLattice lb = NumberLattice::from(b);
      return la.subsumes(lb) && !lb.subsumes(la);
    }

    return false;
  }

  LogicalResult matchAndRewrite(StructOp op,
                                PatternRewriter &rewriter) const override {
    ValueRange fieldResults = op.getFieldResults();
    if (fieldResults.size() < 2)
      return rewriter.notifyMatchFailure(op, "fewer than two validators");

    SmallVector<Value> updated(fieldResults.begin(), fieldResults.end());
    SmallVector<Value> replaced;
    bool changed = false;

    for (size_t i = 0, e = updated.size(); i < e; ++i) {
      for (size_t j = 0; j < e; ++j) {
        if (i == j)
          continue;
        if (strictlyStronger(updated[j], updated[i])) {
          replaced.push_back(updated[i]);
          updated[i] = updated[j];
          changed = true;
          break;
        }
      }
    }

    if (!changed)
      return rewriter.notifyMatchFailure(op, "no subsumed validator");

    rewriter.modifyOpInPlace(
        op, [&] { op.getFieldResultsMutable().assign(updated); });

    SetVector<Operation *> dead;
    for (Value value : replaced) {
      Operation *definingOp = value.getDefiningOp();
      if (definingOp && definingOp->use_empty())
        dead.insert(definingOp);
    }
    for (Operation *definingOp : dead)
      rewriter.eraseOp(definingOp);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Patterns: local constraint normalization
//===----------------------------------------------------------------------===//

/// `min_length = 0` is vacuously true for every JSON string.
struct NormalizeStringConstraints final : OpRewritePattern<ValidateStringOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ValidateStringOp op,
                                PatternRewriter &rewriter) const override {
    IntegerAttr minLength = op.getMinLengthAttr();
    if (!minLength || minLength.getInt() != 0)
      return rewriter.notifyMatchFailure(op, "no vacuous constraint");

    rewriter.modifyOpInPlace(op, [&] { op.removeMinLengthAttr(); });
    return success();
  }
};

/// `multiple_of = 1.0` is implied by `integral`.
struct NormalizeNumberConstraints final : OpRewritePattern<ValidateNumberOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ValidateNumberOp op,
                                PatternRewriter &rewriter) const override {
    FloatAttr multipleOf = op.getMultipleOfAttr();
    if (!op.getIntegral() || !multipleOf ||
        multipleOf.getValueAsDouble() != 1.0)
      return rewriter.notifyMatchFailure(op, "no redundant constraint");

    rewriter.modifyOpInPlace(op, [&] { op.removeMultipleOfAttr(); });
    return success();
  }
};

//===----------------------------------------------------------------------===//
// The pass
//===----------------------------------------------------------------------===//

struct SchemaCanonicalizerPass final
    : PassWrapper<SchemaCanonicalizerPass, OperationPass<func::FuncOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SchemaCanonicalizerPass)

  SchemaCanonicalizerPass() = default;
  SchemaCanonicalizerPass(const SchemaCanonicalizerPass &other)
      : PassWrapper(other) {}

  StringRef getArgument() const final { return "schema-canonicalize"; }

  StringRef getDescription() const final {
    return "Eliminate redundant and subsumed `schema` validation operations";
  }

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<schema::SchemaDialect, arith::ArithDialect,
                    func::FuncDialect>();
  }

  Option<int64_t> maxIterations{
      *this, "max-iterations",
      llvm::cl::desc("Maximum number of greedy rewrite iterations"),
      llvm::cl::init(10)};

  Option<bool> normalizeConstraints{
      *this, "normalize-constraints",
      llvm::cl::desc("Also strip vacuous / implied constraint attributes"),
      llvm::cl::init(true)};

  Statistic numValidationsRemoved{
      this, "validations-removed",
      "Number of `schema` validation operations eliminated"};

  void runOnOperation() override {
    func::FuncOp function = getOperation();
    MLIRContext *context = &getContext();

    const int64_t before = countValidations(function);

    RewritePatternSet patterns(context);
    populateSchemaCanonicalizationPatterns(patterns);
    if (normalizeConstraints)
      populateSchemaConstraintNormalizationPatterns(patterns);

    FrozenRewritePatternSet frozen(std::move(patterns));
    if (failed(applyGreedily(function, frozen, maxIterations))) {
      function.emitError("`schema-canonicalize` failed to converge after ")
          << maxIterations << " iterations";
      return signalPassFailure();
    }

    numValidationsRemoved += before - countValidations(function);
  }

private:
  static int64_t countValidations(func::FuncOp function) {
    int64_t count = 0;
    function.walk([&](Operation *op) {
      if (isa<ValidateStringOp, ValidateNumberOp>(op))
        ++count;
    });
    return count;
  }
};

} // namespace

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

void mlir::schema::populateSchemaCanonicalizationPatterns(
    RewritePatternSet &patterns) {
  patterns.add<FoldConjunctiveValidations, DropSubsumedStructValidators>(
      patterns.getContext());
}

void mlir::schema::populateSchemaConstraintNormalizationPatterns(
    RewritePatternSet &patterns) {
  patterns.add<NormalizeStringConstraints, NormalizeNumberConstraints>(
      patterns.getContext());
}

std::unique_ptr<Pass> mlir::schema::createSchemaCanonicalizerPass() {
  return std::make_unique<SchemaCanonicalizerPass>();
}

void mlir::schema::registerSchemaPasses() {
  PassRegistration<SchemaCanonicalizerPass>();      // --schema-canonicalize
  registerSchemaLoweringPasses();
}