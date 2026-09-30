# Front-end trace format 🧾

`schema-translate --import-json-schema --emit-trace=<file.json>` writes one
JSON document describing a run of the front end: the tokens, the AST, the
diagnostics and the IR after each pipeline stage. Every item carries the
source range it came from, and every op lists the AST nodes it came from. The
viewer in `compiler-tooling-lab` consumes this file.

This document describes **version 1**.

```bash
build/bin/schema-translate --import-json-schema \
    examples/person/person.schema.json --emit-trace=person.trace.json
```

---

## 🧭 Conventions

- **Positions** are objects `{ "offset", "line", "column" }`. `offset` is a
  0-based byte offset into `source.text`; `line` and `column` are 1-based, and
  the column counts bytes (as `FileLineColLoc` and `llvm::SourceMgr` do).
- **Ranges** are `{ "begin": Position, "end": Position }` and are half-open:
  `end` is one past the last byte.
- **AST ids** are assigned in pre-order starting at 0 (the root schema).
- Keys may be added within a version. A consumer should ignore keys it does
  not know and reject a `version` it does not support.

---

## 📦 Top level

```json
{
  "format": "json-schema-mlir-trace",
  "version": 1,
  "source": { "file": "person.schema.json", "text": "{\n  ..." },
  "tokens": [Token, ...],
  "ast": Node | null,
  "diagnostics": [Diagnostic, ...],
  "stages": [Stage, ...]
}
```

| Key           | Contents                                                             |
| ------------- | -------------------------------------------------------------------- |
| `format`      | Always `"json-schema-mlir-trace"`                                    |
| `version`     | Integer, bumped on incompatible changes                              |
| `source`      | The input file name (as given on the command line) and its full text |
| `tokens`      | The whole token stream, ending with an `eof` token                   |
| `ast`         | The root schema node; `null` if nothing could be parsed              |
| `diagnostics` | Every error, warning and remark, in emission order                   |
| `stages`      | The IR after each stage that ran                                     |

The trace is written even when the run fails, and holds whatever was
produced: after a syntax error there are tokens, a partial AST and the
diagnostics; after a semantic error there is also a full AST; `stages` is
filled only when the import succeeds.

With `--split-input-file`, each chunk overwrites the trace, so the file
describes the last chunk.

---

## 🔤 Tokens

```json
{ "kind": "string", "text": "\"type\"", "range": Range }
```

`kind` is one of `l_brace`, `r_brace`, `l_square`, `r_square`, `colon`,
`comma`, `string`, `number`, `true`, `false`, `null`, `eof` and `error`.
`text` is the raw source text, quotes and escapes included (empty for `eof`).
An `error` token covers the bytes the lexer rejected; the matching entry is
in `diagnostics`.

---

## 🌳 AST nodes

Every node has:

| Key        | Contents                                                           |
| ---------- | ------------------------------------------------------------------ |
| `id`       | Pre-order id                                                       |
| `kind`     | `"schema"`, `"property"` or `"keyword"`                            |
| `range`    | The whole node: `{ ... }` for a schema, key to value end otherwise |
| `children` | Child nodes in source order (possibly empty)                       |

Kind-specific keys:

- **`schema`**: a JSON Schema object. Its children are its keywords.
- **`property`**: one member of `properties`. `name` is the decoded property
  name and `nameRange` the range of its key. Its single child is the
  property's schema.
- **`keyword`**: `keyword` is the key as written (`"minimum"`), `keyRange`
  the range of the key alone, and `category` one of:

| `category`    | Keywords                                                                                                           | `value`                                                |
| ------------- | ------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------ |
| `type`        | `type`                                                                                                             | Array of type names                                    |
| `properties`  | `properties`                                                                                                       | — (children are `property` nodes)                      |
| `required`    | `required`                                                                                                         | Array of property names                                |
| `allOf`       | `allOf`                                                                                                            | — (children are `schema` nodes)                        |
| `number`      | `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf`                                         | Number, plus `spelling` as written                     |
| `length`      | `minLength`, `maxLength`                                                                                           | Integer                                                |
| `string`      | `pattern`, `format`                                                                                                | Decoded string                                         |
| `annotation`  | `$schema`, `$id`, `$comment`, `title`, `description`, `default`, `examples`, `deprecated`, `readOnly`, `writeOnly` | Decoded string, or absent if the value is not a string |
| `unsupported` | Anything else                                                                                                      | —                                                      |

A keyword whose value had the wrong shape is left out of the AST (its
diagnostic remains), so ids can have gaps after a parse error.

---

## 🩺 Diagnostics

```json
{
  "severity": "error",
  "message": "no number satisfies both 'minimum' (5) and 'maximum' (3)",
  "range": Range | null,
  "notes": [Diagnostic, ...]
}
```

`severity` is `error`, `warning`, `remark` or `note` (notes appear only
inside `notes`). `range` is the token that starts at the diagnostic's
location, or an empty range at that position if no token starts there; it is
`null` for a diagnostic without a location in the source file.

---

## 🧱 Stages

```json
{ "name": "import", "ir": "module {\n  func.func @validate_person(...", "ops": [Op, ...] }
```

| `name`                | IR                                                     |
| --------------------- | ------------------------------------------------------ |
| `import`              | The importer's output (what `schema-translate` prints) |
| `schema-canonicalize` | The same module after `--schema-canonicalize`          |

`ir` is the module in MLIR's custom assembly form, exactly as the tools print
it. `ops` lists every operation in the module in pre-order,
including `builtin.module` itself:

```json
{
  "id": 3,
  "name": "schema.validate_number",
  "parent": 1,
  "irPos": { "line": 5, "column": 5 },
  "astNodes": [14, 15, 17, 18, 20, 21],
  "ranges": [Range, ...]
}
```

| Key        | Contents                                                             |
| ---------- | -------------------------------------------------------------------- |
| `id`       | Pre-order index within this stage (ids are not stable across stages) |
| `name`     | The operation name                                                   |
| `parent`   | `id` of the enclosing op, `null` for the module                      |
| `irPos`    | 1-based line and column of the op in `ir`, or `null`                 |
| `astNodes` | Ids of the AST nodes the op came from                                |
| `ranges`   | The `range` of each node in `astNodes`, in the same order            |

### 🔗 How ops map to AST nodes

The importer gives each op the `FileLineColLoc` of the node that produced it:

| Op                                           | Node                                        |
| -------------------------------------------- | ------------------------------------------- |
| `schema.validate_string` / `validate_number` | The keyword (`type` for a type guard)       |
| `schema.struct`                              | `properties` (else `type`, else `required`) |
| `arith.andi`, `arith.constant true`          | The schema object whose conjuncts it joins  |
| `func.func`, `func.return`, `builtin.module` | The root schema                             |

No two AST nodes start at the same position, so a location identifies one
node. Passes keep locations: when `--schema-canonicalize` fuses several ops
it gives the result a `FusedLoc` of theirs, and the trace lists every node
in it. That is how the fused `age` validator in the Person example points
back at all six keywords it came from.
