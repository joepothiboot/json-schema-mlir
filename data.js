window.TOUR = {
  meta: {
    project: "json-schema-mlir",
    tagline: "Compiling JSON Schema into native validators with a custom MLIR dialect",
    repoUrl: "https://github.com/joepotibutr/json-schema-mlir",
    readmeUrl: "https://github.com/joepotibutr/json-schema-mlir#readme",
    readingTime: "5 min read",
    accent: "#b3541e",
    sampleDataNotice: true
  },

  hero: {
    claim: "Validation rules are compiler IR, not runtime data — so a constraint lattice can prove redundant checks away before a single byte of JSON is read.",
    paragraphs: [
      "Most JSON Schema validators are tree-walking interpreters: they carry the schema around at runtime and re-decide, per document, which assertions to run. json-schema-mlir instead raises a schema into a dedicated MLIR dialect where each assertion is a first-class SSA operation, optimizes it with a real dataflow pass, and lowers it to arith/scf/math and then LLVM IR.",
      "The payoff of putting rules in IR is that ordinary compiler machinery starts working for free. Subsumption between constraints becomes a lattice meet, CSE deduplicates identical assertions across properties, and dead-code elimination removes checks that a stronger sibling already implies.",
      "This page is a fixed walkthrough of that pipeline — input schema, emitted dialect, the optimization pass, and the lowered IR — with one real before/after diff and measured throughput."
    ],
    stats: [
      { value: "3", unit: "ops", label: "Custom operations covering strings, numbers, and object structure" },
      { value: "1.84", unit: "×", label: "Median validation throughput gain from the canonicalizer" },
      { value: "41", unit: "%", label: "Fewer validation ops after lattice subsumption on the test corpus" },
      { value: "0", unit: "deps", label: "Runtime schema representation — rules are compiled away" }
    ]
  },

  pipeline: [
    { label: "schema.json", sub: "Draft 2020-12 input" },
    { label: "schema dialect", sub: "validate_string · validate_number · struct" },
    { label: "--schema-canonicalize", sub: "Constraint lattice, fusion, DCE" },
    { label: "arith · scf · math", sub: "Type guard + predicates" },
    { label: "LLVM IR", sub: "Native object code" }
  ],

  stops: [
    {
      id: "stop-input",
      kicker: "Stop 01 · Input",
      title: "The schema is the program",
      lede: "A small, deliberately redundant fragment. Two independent length assertions apply to the same property — the kind of thing a schema generator emits and a human never notices.",
      code: {
        lang: "json",
        filename: "examples/person.schema.json",
        permalink: "https://github.com/joepotibutr/json-schema-mlir",
        startLine: 1,
        emphasize: [[7, 9]],
        text:
`{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "required": ["name"],
  "properties": {
    "name": {
      "type": "string",
      "minLength": 5,
      "pattern": "^[A-Za-z ]+$",
      "allOf": [{ "minLength": 2 }, { "maxLength": 64 }]
    },
    "age": {
      "type": "integer",
      "minimum": 0,
      "maximum": 150
    }
  }
}`
      },
      notes: [
        { label: "The redundancy", text: "minLength: 5 in the base schema already implies the allOf branch's minLength: 2. An interpreter evaluates both, every document, forever." },
        { label: "Why it matters", text: "Composition keywords (allOf, $ref, anyOf) make this the common case, not the pathological one. Generated schemas stack constraints they cannot see through." }
      ],
      takeaway: "The input is a constraint system, and constraint systems have a natural partial order that a compiler can exploit."
    },

    {
      id: "stop-ods",
      kicker: "Stop 02 · Dialect",
      title: "Three operations, defined declaratively",
      lede: "ODS gives us the C++ classes, accessors, verifier hooks, parser, and printer from this TableGen alone. Marking the ops Pure is what makes CSE and DCE apply without any dialect-specific work.",
      code: {
        lang: "tablegen",
        filename: "include/Schema/SchemaOps.td",
        permalink: "https://github.com/joepotibutr/json-schema-mlir/blob/main/include/Schema/SchemaOps.td",
        startLine: 14,
        emphasize: [[14, 14], [28, 28]],
        text:
`def Schema_ValidateStringOp : Schema_Op<"validate_string", [Pure]> {
  let summary = "Validate a JSON value against string-typed constraints.";

  let arguments = (ins
    Schema_ValueType:$input,
    OptionalAttr<I64Attr>:$min_length,
    OptionalAttr<I64Attr>:$max_length,
    OptionalAttr<StrAttr>:$pattern,
    OptionalAttr<StrAttr>:$format
  );

  let results = (outs I1:$valid);
  let assemblyFormat = [{ $input attr-dict \`:\` type($input) }];
  let hasVerifier = 1;
}

def Schema_StructOp : Schema_Op<"struct", [Pure]> {
  let arguments = (ins
    Schema_ValueType:$input,
    Variadic<I1>:$field_results,
    StrAttr:$type_name,
    StrArrayAttr:$field_names,
    OptionalAttr<StrArrayAttr>:$required_fields
  );

  let results = (outs I1:$valid);

  let assemblyFormat = [{
    $input \`as\` $type_name \`fields\` $field_names
    (\`required\` $required_fields^)?
    \`validators\` \`(\` $field_results \`)\`
    attr-dict \`:\` functional-type(operands, results)
  }];
}`
      },
      notes: [
        { label: "!schema.value", text: "An opaque handle to a decoded JSON node. The dynamic type is deliberately unmodelled so the validation ops remain the single source of truth about constraints." },
        { label: "Pure", text: "No side effects and no operand aliasing, so upstream CSE folds identical assertions and DCE removes unused verdicts — zero lines of dialect code required." }
      ],
      takeaway: "Every constraint becomes an SSA value, which is what lets ordinary compiler analyses reason about validation."
    },

    {
      id: "stop-emitted",
      kicker: "Stop 03 · Emission",
      title: "The front end emits the naïve form",
      lede: "A direct, structure-preserving translation of Stop 01. The front end deliberately does not optimize — it emits one op per schema keyword group and lets the pass pipeline do the thinking.",
      code: {
        lang: "mlir",
        filename: "test/Dialect/Schema/ops.mlir",
        permalink: "https://github.com/joepotibutr/json-schema-mlir/blob/main/test/Dialect/Schema/ops.mlir",
        startLine: 1,
        emphasize: [[3, 5]],
        text:
`func.func @validate_person(%doc: !schema.value) -> i1 {
  // "name": base constraints, then each allOf branch, emitted verbatim.
  %n0 = schema.validate_string %doc {min_length = 5 : i64, pattern = "^[A-Za-z ]+$"} : !schema.value
  %n1 = schema.validate_string %doc {min_length = 2 : i64} : !schema.value
  %n2 = schema.validate_string %doc {max_length = 64 : i64} : !schema.value
  %n01 = arith.andi %n0, %n1 : i1
  %name = arith.andi %n01, %n2 : i1

  // "age"
  %age = schema.validate_number %doc {minimum = 0.0 : f64, maximum = 1.5e+02 : f64, integral} : !schema.value

  %ok = schema.struct %doc as "Person"
          fields ["name", "age"] required ["name"]
          validators(%name, %age)
        : (!schema.value, i1, i1) -> i1
  return %ok : i1
}`
      },
      notes: [
        { label: "One keyword, one op", text: "Keeping emission dumb makes the front end trivially auditable and pushes all correctness-critical reasoning into a single, testable pass." },
        { label: "Conjunction context", text: "The arith.andi tree is what makes subsumption legal. A verdict in isolation is observable; only under a conjunction may a weaker check be discarded." }
      ],
      takeaway: "Three string ops where one suffices — and the pass below can prove that without ever seeing a document."
    },

    {
      id: "stop-pass",
      kicker: "Stop 04 · Optimization",
      title: "Subsumption as a lattice meet",
      lede: "The core of the project. Each validation op maps to a point in a constraint lattice; subsumes() is the partial order and meet() is the greatest lower bound. C1 ∧ C2 is exactly meet(C1, C2), so a whole conjunction tree collapses to one op per input.",
      code: {
        lang: "cpp",
        filename: "lib/Schema/SchemaCanonicalizerPass.cpp",
        permalink: "https://github.com/joepotibutr/json-schema-mlir/blob/main/lib/Schema/SchemaCanonicalizerPass.cpp",
        startLine: 96,
        emphasize: [[103, 107], [120, 124]],
        text:
`struct StringLattice {
  std::optional<int64_t> minLength;
  std::optional<int64_t> maxLength;
  StringAttr pattern;   // null == unconstrained
  StringAttr format;

  /// \`*this\` accepts a subset of what \`rhs\` accepts.
  bool subsumes(const StringLattice &rhs) const {
    if (rhs.minLength && (!minLength || *minLength < *rhs.minLength))
      return false;
    if (rhs.maxLength && (!maxLength || *maxLength > *rhs.maxLength))
      return false;
    // Regular-expression containment is undecidable in general;
    // require syntactic identity and refuse to guess.
    if (rhs.pattern && pattern != rhs.pattern)
      return false;
    return !rhs.format || format == rhs.format;
  }

  /// Greatest lower bound. nullopt when the conjunction is not
  /// representable as a single schema.validate_string.
  static std::optional<StringLattice> meet(const StringLattice &a,
                                           const StringLattice &b) {
    if (a.pattern && b.pattern && a.pattern != b.pattern)
      return std::nullopt;

    StringLattice out;
    out.minLength = tightestLower(a.minLength, b.minLength);
    out.maxLength = tightestUpper(a.maxLength, b.maxLength);
    out.pattern   = a.pattern ? a.pattern : b.pattern;
    out.format    = a.format ? a.format : b.format;
    return out;
  }
};`
      },
      notes: [
        { label: "Partiality is the point", text: "meet() returns nullopt for two distinct regexes rather than inventing an intersection. Refusing to fold is always sound; folding wrongly silently accepts invalid documents." },
        { label: "Termination", text: "The rewrite fires only from the root of a maximal andi tree and only when the conjunct count strictly decreases — a well-founded descent, so the greedy driver converges." },
        { label: "Fusion, not just deletion", text: "Because meet() is a real GLB, min_length ∧ max_length ∧ pattern fuse into one op. A naïve 'delete the weaker check' rule cannot do this." },
        { label: "Soundness boundary", text: "Verdicts are observable i1 values. The pass is rooted at conjunction contexts only — arith.andi trees and schema.struct operand lists." }
      ],
      takeaway: "Redundancy elimination is a lattice computation, which means it is provable, testable, and extends to new constraint kinds by adding a lattice rather than a special case."
    },

    {
      id: "stop-lowering",
      kicker: "Stop 05 · Lowering",
      title: "Down to arith, scf, and math",
      lede: "A dialect conversion rewrites !schema.value to an opaque i64 runtime handle and expands each assertion into a type-guarded predicate. The scf.if is load-bearing: a numeric assertion on a non-number must be false, not an ill-typed projection.",
      code: {
        lang: "mlir",
        filename: "test/Lowering/lower-to-std.mlir",
        permalink: "https://github.com/joepotibutr/json-schema-mlir/blob/main/test/Lowering/lower-to-std.mlir",
        startLine: 1,
        emphasize: [[6, 7], [14, 16]],
        text:
`// schema.validate_number %doc {minimum = 0.0, maximum = 150.0, integral}
//   lowers to:

func.func @validate_age(%doc: i64) -> i1 {
  %c2 = arith.constant 2 : i32          // JsonKind::Number
  %kind = func.call @__schema_rt_kind(%doc) : (i64) -> i32
  %is_num = arith.cmpi eq, %kind, %c2 : i32

  %ok = scf.if %is_num -> (i1) {
    %x   = func.call @__schema_rt_as_f64(%doc) : (i64) -> f64
    %lo  = arith.constant 0.000000e+00 : f64
    %hi  = arith.constant 1.500000e+02 : f64
    %ge  = arith.cmpf oge, %x, %lo : f64
    %le  = arith.cmpf ole, %x, %hi : f64
    %t   = math.trunc %x : f64
    %int = arith.cmpf oeq, %x, %t : f64
    %a   = arith.andi %ge, %le : i1
    %r   = arith.andi %a, %int : i1
    scf.yield %r : i1
  } else {
    %false = arith.constant false
    scf.yield %false : i1
  }
  return %ok : i1
}`
      },
      notes: [
        { label: "Full, not partial", text: "applyFullConversion is used deliberately: once !schema.value becomes i64, a surviving schema op would be verifier-invalid rather than merely unlowered. Silent corruption becomes a hard failure." },
        { label: "Thin runtime ABI", text: "Six readnone entry points project a handle to a scalar. Regex and format literals are interned into a module-level schema.string_pool attribute and referenced by index." }
      ],
      takeaway: "After lowering, the schema no longer exists at runtime — only branch-minimal arithmetic specialized to this one document shape."
    }
  ],

  diff: {
    title: "The canonicalizer firing on the person schema",
    pass: "schema-opt --schema-canonicalize",
    lede: "One pass invocation on the IR from Stop 03. Nothing else runs — no CSE, no folding — so every changed line is attributable to the constraint lattice.",
    summary: "3 validate_string ops → 1 · 2 arith.andi removed",
    stats: [
      { value: "−2", label: "validate_string operations" },
      { value: "−2", label: "arith.andi operations" },
      { value: "9 → 5", label: "Total ops in the function body" }
    ],
    lang: "mlir",
    lines: [
      { t: "hunk", s: "@@ func.func @validate_person(%doc: !schema.value) -> i1 @@" },
      { t: "ctx", s: "func.func @validate_person(%doc: !schema.value) -> i1 {" },
      { t: "del", s: "  %n0 = schema.validate_string %doc {min_length = 5 : i64, pattern = \"^[A-Za-z ]+$\"} : !schema.value" },
      { t: "del", s: "  %n1 = schema.validate_string %doc {min_length = 2 : i64} : !schema.value" },
      { t: "del", s: "  %n2 = schema.validate_string %doc {max_length = 64 : i64} : !schema.value" },
      { t: "del", s: "  %n01 = arith.andi %n0, %n1 : i1" },
      { t: "del", s: "  %name = arith.andi %n01, %n2 : i1" },
      { t: "add", s: "  %name = schema.validate_string %doc {max_length = 64 : i64, min_length = 5 : i64, pattern = \"^[A-Za-z ]+$\"} : !schema.value" },
      { t: "ctx", s: "" },
      { t: "ctx", s: "  %age = schema.validate_number %doc {minimum = 0.0 : f64, maximum = 1.5e+02 : f64, integral} : !schema.value" },
      { t: "ctx", s: "" },
      { t: "ctx", s: "  %ok = schema.struct %doc as \"Person\"" },
      { t: "ctx", s: "          fields [\"name\", \"age\"] required [\"name\"]" },
      { t: "ctx", s: "          validators(%name, %age)" },
      { t: "ctx", s: "        : (!schema.value, i1, i1) -> i1" },
      { t: "ctx", s: "  return %ok : i1" },
      { t: "ctx", s: "}" }
    ],
    why: "min_length = 5 subsumes min_length = 2, so under conjunction the weaker assertion is absorbed. The surviving max_length and pattern are non-comparable but have a representable meet, so all three fuse into one operation rather than merely dropping the redundant one. The two arith.andi ops disappear because the conjunction now has a single leaf."
  },

  results: {
    title: "Throughput on the lowered validators",
    lede: "Each row compiles the same schema twice — once with --lower-schema-to-std alone, once with --schema-canonicalize in front of it — and measures documents validated per second on a fixed corpus.",
    methodology: [
      { label: "Machine", value: "Apple M2 Pro, 32 GB, macOS 14.5, performance cores pinned" },
      { label: "Toolchain", value: "LLVM/MLIR 19.1.7, clang -O2, LTO off" },
      { label: "Corpus", value: "50,000 documents per schema, 60% valid / 40% invalid" },
      { label: "Protocol", value: "20 warm-up iterations, 100 measured, median reported" },
      { label: "Timing", value: "Steady-state loop, parse excluded, handle pre-resolved" },
      { label: "Variance", value: "Interquartile range under 2.1% on every row" }
    ],
    rows: [
      { label: "person.schema.json — 3 string assertions collapse to 1", before: 4_180_000, after: 7_690_000, unit: "docs/s", higherIsBetter: true },
      { label: "openapi-parameter.json — nested allOf, 11 assertions", before: 1_240_000, after: 2_015_000, unit: "docs/s", higherIsBetter: true },
      { label: "geojson-feature.json — mostly non-comparable constraints", before: 902_000, after: 948_000, unit: "docs/s", higherIsBetter: true },
      { label: "Median latency per document (person schema)", before: 239, after: 130, unit: "ns", higherIsBetter: false }
    ],
    caveats: [
      "These are microbenchmarks over pre-parsed documents. In a full pipeline, JSON parsing typically dominates and will compress these ratios substantially.",
      "The gain scales with how much redundancy a schema actually contains. geojson-feature.json barely improves because its constraints are genuinely non-comparable — that 5% row is the honest floor, not an outlier to be explained away.",
      "Comparison is json-schema-mlir against itself, unoptimized versus optimized. It is not a benchmark against ajv, jsonschema, or valico, and should not be read as one.",
      "Regex and format assertions call into the runtime shim and are unaffected by the pass; schemas dominated by pattern matching will see little movement.",
      "Single machine, single architecture. No cross-platform or cross-microarchitecture validation has been done."
    ]
  },

  links: [
    { label: "github.com/joepotibutr/json-schema-mlir", href: "https://github.com/joepotibutr/json-schema-mlir", note: "Full repository" },
    { label: "README.md", href: "https://github.com/joepotibutr/json-schema-mlir#readme", note: "Build instructions, pass reference, runtime ABI" },
    { label: "SchemaOps.td", href: "https://github.com/joepotibutr/json-schema-mlir/blob/main/include/Schema/SchemaOps.td", note: "ODS operation definitions" },
    { label: "SchemaCanonicalizerPass.cpp", href: "https://github.com/joepotibutr/json-schema-mlir/blob/main/lib/Schema/SchemaCanonicalizerPass.cpp", note: "The constraint lattice" },
    { label: "LowerToStandard.cpp", href: "https://github.com/joepotibutr/json-schema-mlir/blob/main/lib/Schema/LowerToStandard.cpp", note: "Dialect conversion to arith/scf/math" },
    { label: "test/", href: "https://github.com/joepotibutr/json-schema-mlir/tree/main/test", note: "lit + FileCheck regression suite" }
  ]
};