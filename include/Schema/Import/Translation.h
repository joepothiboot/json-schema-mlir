//===- Translation.h - JSON Schema import translation -----------*- C++ -*-===//
#ifndef SCHEMA_IMPORT_TRANSLATION_H
#define SCHEMA_IMPORT_TRANSLATION_H

namespace mlir::schema {

/// Registers `--import-json-schema` (with `--dump-tokens`, `--dump-ast` and
/// `--emit-trace=<file>`) for use with `mlirTranslateMain`.
void registerImportJsonSchemaTranslation();

} // namespace mlir::schema

#endif // SCHEMA_IMPORT_TRANSLATION_H
