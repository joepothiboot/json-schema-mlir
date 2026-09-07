#include "Schema/SchemaOps.h"

#include "Schema/SchemaDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Regex.h"

#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::schema;

#define GET_OP_CLASSES
#include "Schema/SchemaOps.cpp.inc"

//===----------------------------------------------------------------------===//
// ValidateStringOp
//===----------------------------------------------------------------------===//

LogicalResult ValidateStringOp::verify() {
  std::optional<int64_t> minLength;
  if (IntegerAttr attr = getMinLengthAttr()) {
    minLength = attr.getInt();
    if (*minLength < 0)
      return emitOpError("'min_length' must be non-negative, got ") << *minLength;
  }

  if (IntegerAttr attr = getMaxLengthAttr()) {
    int64_t maxLength = attr.getInt();
    if (maxLength < 0)
      return emitOpError("'max_length' must be non-negative, got ") << maxLength;
    if (minLength && maxLength < *minLength)
      return emitOpError("'max_length' (")
             << maxLength << ") must be >= 'min_length' (" << *minLength << ")";
  }

  if (StringAttr attr = getPatternAttr()) {
    StringRef pattern = attr.getValue();
    if (pattern.empty())
      return emitOpError("'pattern' must not be empty");
    std::string error;
    llvm::Regex regex(pattern);
    if (!regex.isValid(error))
      return emitOpError("'pattern' is not a valid regular expression: ") << error;
  }

  if (StringAttr attr = getFormatAttr(); attr && attr.getValue().empty())
    return emitOpError("'format' must not be empty");

  return success();
}

//===----------------------------------------------------------------------===//
// ValidateNumberOp
//===----------------------------------------------------------------------===//

LogicalResult ValidateNumberOp::verify() {
  std::optional<double> minimum;
  std::optional<double> maximum;

  if (FloatAttr attr = getMinimumAttr())
    minimum = attr.getValueAsDouble();
  if (FloatAttr attr = getMaximumAttr())
    maximum = attr.getValueAsDouble();

  if (minimum && maximum && *minimum > *maximum)
    return emitOpError("'minimum' (")
           << *minimum << ") must be <= 'maximum' (" << *maximum << ")";

  if (getExclusiveMinimum() && !minimum)
    return emitOpError("'exclusive_minimum' requires 'minimum' to be present");
  if (getExclusiveMaximum() && !maximum)
    return emitOpError("'exclusive_maximum' requires 'maximum' to be present");

  if (FloatAttr attr = getMultipleOfAttr()) {
    double multipleOf = attr.getValueAsDouble();
    if (!(multipleOf > 0.0))
      return emitOpError("'multiple_of' must be strictly positive, got ")
             << multipleOf;
  }

  return success();
}

//===----------------------------------------------------------------------===//
// StructOp
//===----------------------------------------------------------------------===//

LogicalResult StructOp::verify() {
  if (getTypeName().empty())
    return emitOpError("'type_name' must not be empty");

  ArrayAttr fieldNames = getFieldNames();
  if (fieldNames.size() != getFieldResults().size())
    return emitOpError("expects one validator operand per declared field: ")
           << fieldNames.size() << " field name(s) vs "
           << getFieldResults().size() << " validator operand(s)";

  llvm::SmallDenseSet<StringRef> declared;
  for (Attribute attr : fieldNames) {
    auto name = dyn_cast<StringAttr>(attr);
    if (!name || name.getValue().empty())
      return emitOpError("'field_names' must contain non-empty string attributes");
    if (!declared.insert(name.getValue()).second)
      return emitOpError("duplicate field name '") << name.getValue() << "'";
  }

  if (ArrayAttr required = getRequiredFieldsAttr()) {
    llvm::SmallDenseSet<StringRef> seenRequired;
    for (Attribute attr : required) {
      auto name = dyn_cast<StringAttr>(attr);
      if (!name)
        return emitOpError("'required_fields' must contain string attributes");
      if (!declared.contains(name.getValue()))
        return emitOpError("required field '")
               << name.getValue() << "' is not declared in 'field_names'";
      if (!seenRequired.insert(name.getValue()).second)
        return emitOpError("duplicate required field '") << name.getValue() << "'";
    }
  }

  return success();
}