"""JSON Schema constraint lattices and validators in Mojo.

A library version of what the `schema` dialect does at compile time:

- `subsumes` and `meet` are the lattice operations `--schema-canonicalize`
  uses (lib/Schema/SchemaCanonicalizerPass.cpp), with the same rules.
- `validate` has the same semantics as the code `--lower-schema-to-std`
  emits (lib/Schema/LowerToStandard.cpp).

The tests use this to check the canonicalizer's soundness property directly:
for every value, `meet(a, b).validate(x) == a.validate(x) and b.validate(x)`.
"""

from .lattice import NumberConstraints, StringConstraints, code_point_length
