"""String and number constraint lattices.

Each struct is one point in a lattice ordered by strictness. `a.subsumes(b)`
means every value `a` accepts, `b` also accepts, so `b` is redundant next to
`a`. `meet(a, b)` is a single constraint equivalent to `a and b`, or `None`
when no single constraint can express it (two different regexes, or
`multipleOf` divisors where neither is a multiple of the other).

Validation takes an already-typed value. The JSON type guard (a string
constraint on a number is `false`) is the caller's job, as it is the
`scf.if` around each lowered validator.
"""

from std.math import trunc


# --- strings ----------------------------------------------------------------


struct StringConstraints(Copyable, Movable):
    """`minLength`, `maxLength`, `pattern` and `format` for one value.

    Lengths count Unicode code points, as JSON Schema requires (and as
    `__schema_rt_str_len` does), not UTF-8 bytes.
    """

    var min_length: Optional[Int]
    var max_length: Optional[Int]
    var pattern: Optional[String]
    var format: Optional[String]

    def __init__(out self):
        """The top element: accepts every string."""
        self.min_length = None
        self.max_length = None
        self.pattern = None
        self.format = None

    def with_min_length(self, n: Int) -> Self:
        var r = self.copy()
        r.min_length = n
        return r^

    def with_max_length(self, n: Int) -> Self:
        var r = self.copy()
        r.max_length = n
        return r^

    def with_pattern(self, regex: String) -> Self:
        var r = self.copy()
        r.pattern = regex
        return r^

    def with_format(self, name: String) -> Self:
        var r = self.copy()
        r.format = name
        return r^

    def subsumes(self, other: Self) -> Bool:
        """True when `self` is at least as strict as `other` on every axis."""
        if other.min_length:
            if not self.min_length or self.min_length.value() < other.min_length.value():
                return False
        if other.max_length:
            if not self.max_length or self.max_length.value() > other.max_length.value():
                return False
        if not _covers(self.pattern, other.pattern):
            return False
        return _covers(self.format, other.format)

    @staticmethod
    def meet(a: Self, b: Self) -> Optional[Self]:
        """The single constraint equivalent to `a and b`, if one exists.

        Regex containment is not decided, so two different patterns (or
        formats) have no representable meet.
        """
        if a.pattern and b.pattern and a.pattern.value() != b.pattern.value():
            return None
        if a.format and b.format and a.format.value() != b.format.value():
            return None
        var r = Self()
        r.min_length = _max_opt(a.min_length, b.min_length)
        r.max_length = _min_opt(a.max_length, b.max_length)
        r.pattern = a.pattern.copy() if a.pattern else b.pattern.copy()
        r.format = a.format.copy() if a.format else b.format.copy()
        return r^

    def is_vacuous(self) -> Bool:
        """True when this accepts every string (a pure type check)."""
        return (
            not self.min_length
            and not self.max_length
            and not self.pattern
            and not self.format
        )

    def validate(self, s: String) raises -> Bool:
        """Checks `s` against the length bounds.

        Raises:
            If a `pattern` or `format` is set: this library has no regex or
            format engine (the compiled validator calls into the runtime
            for those).
        """
        if self.pattern or self.format:
            raise Error("StringConstraints: pattern/format need the runtime")
        var n = code_point_length(s)
        if self.min_length and n < self.min_length.value():
            return False
        if self.max_length and n > self.max_length.value():
            return False
        return True

    def __eq__(self, other: Self) -> Bool:
        return self.subsumes(other) and other.subsumes(self)

    def __ne__(self, other: Self) -> Bool:
        return not self == other


def code_point_length(s: String) -> Int:
    """Number of Unicode code points in `s` (UTF-8 continuation bytes skipped).
    """
    var n = 0
    for b in s.as_bytes():
        if (b & 0xC0) != 0x80:
            n += 1
    return n


# --- numbers ----------------------------------------------------------------


struct NumberConstraints(Copyable, Movable):
    """`minimum`/`exclusiveMinimum`, `maximum`/`exclusiveMaximum`,
    `multipleOf`, and the `integer` type, for one value.
    """

    var minimum: Optional[Float64]
    var maximum: Optional[Float64]
    var multiple_of: Optional[Float64]
    var exclusive_minimum: Bool
    var exclusive_maximum: Bool
    var integral: Bool

    def __init__(out self):
        """The top element: accepts every number."""
        self.minimum = None
        self.maximum = None
        self.multiple_of = None
        self.exclusive_minimum = False
        self.exclusive_maximum = False
        self.integral = False

    def with_minimum(self, bound: Float64, exclusive: Bool = False) -> Self:
        var r = self.copy()
        r.minimum = bound
        r.exclusive_minimum = exclusive
        return r^

    def with_maximum(self, bound: Float64, exclusive: Bool = False) -> Self:
        var r = self.copy()
        r.maximum = bound
        r.exclusive_maximum = exclusive
        return r^

    def with_multiple_of(self, divisor: Float64) -> Self:
        var r = self.copy()
        r.multiple_of = divisor
        return r^

    def with_integral(self) -> Self:
        var r = self.copy()
        r.integral = True
        return r^

    def subsumes(self, other: Self) -> Bool:
        """True when `self` is at least as strict as `other` on every axis."""
        if not _lower_at_least_as_strict(
            self.minimum, self.exclusive_minimum, other.minimum, other.exclusive_minimum
        ):
            return False
        if not _upper_at_least_as_strict(
            self.maximum, self.exclusive_maximum, other.maximum, other.exclusive_maximum
        ):
            return False
        if other.multiple_of:
            if not self.multiple_of or not _is_integral_multiple_of(
                self.multiple_of.value(), other.multiple_of.value()
            ):
                return False
        return self.integral or not other.integral

    @staticmethod
    def meet(a: Self, b: Self) -> Optional[Self]:
        """The single constraint equivalent to `a and b`, if one exists.

        `multipleOf` only merges when one divisor is an integral multiple of
        the other; a real-valued LCM is not generally representable.
        """
        var r = Self()
        if _lower_at_least_as_strict(
            a.minimum, a.exclusive_minimum, b.minimum, b.exclusive_minimum
        ):
            r.minimum = a.minimum
            r.exclusive_minimum = a.exclusive_minimum
        else:
            r.minimum = b.minimum
            r.exclusive_minimum = b.exclusive_minimum

        if _upper_at_least_as_strict(
            a.maximum, a.exclusive_maximum, b.maximum, b.exclusive_maximum
        ):
            r.maximum = a.maximum
            r.exclusive_maximum = a.exclusive_maximum
        else:
            r.maximum = b.maximum
            r.exclusive_maximum = b.exclusive_maximum

        if a.multiple_of and b.multiple_of:
            var ma = a.multiple_of.value()
            var mb = b.multiple_of.value()
            if _is_integral_multiple_of(ma, mb):
                r.multiple_of = ma
            elif _is_integral_multiple_of(mb, ma):
                r.multiple_of = mb
            else:
                return None
        else:
            r.multiple_of = a.multiple_of if a.multiple_of else b.multiple_of

        r.integral = a.integral or b.integral
        return r^

    def is_vacuous(self) -> Bool:
        """True when this accepts every number (a pure type check)."""
        return (
            not self.minimum
            and not self.maximum
            and not self.multiple_of
            and not self.integral
        )

    def validate(self, x: Float64) -> Bool:
        """Same comparisons as the lowered code: ordered, so NaN fails any
        bound."""
        if self.minimum:
            var lo = self.minimum.value()
            if self.exclusive_minimum:
                if not (x > lo):
                    return False
            elif not (x >= lo):
                return False
        if self.maximum:
            var hi = self.maximum.value()
            if self.exclusive_maximum:
                if not (x < hi):
                    return False
            elif not (x <= hi):
                return False
        if self.multiple_of:
            var q = x / self.multiple_of.value()
            if q != trunc(q):
                return False
        if self.integral and x != trunc(x):
            return False
        return True

    def __eq__(self, other: Self) -> Bool:
        return (
            _opt_eq(self.minimum, other.minimum)
            and _opt_eq(self.maximum, other.maximum)
            and _opt_eq(self.multiple_of, other.multiple_of)
            and self.exclusive_minimum == other.exclusive_minimum
            and self.exclusive_maximum == other.exclusive_maximum
            and self.integral == other.integral
        )

    def __ne__(self, other: Self) -> Bool:
        return not self == other


# --- helpers (same rules as SchemaCanonicalizerPass.cpp) --------------------


def _lower_at_least_as_strict(
    a: Optional[Float64], a_excl: Bool, b: Optional[Float64], b_excl: Bool
) -> Bool:
    if not b:
        return True
    if not a:
        return False
    if a.value() > b.value():
        return True
    if a.value() < b.value():
        return False
    return a_excl or not b_excl


def _upper_at_least_as_strict(
    a: Optional[Float64], a_excl: Bool, b: Optional[Float64], b_excl: Bool
) -> Bool:
    if not b:
        return True
    if not a:
        return False
    if a.value() < b.value():
        return True
    if a.value() > b.value():
        return False
    return a_excl or not b_excl


def _is_integral_multiple_of(a: Float64, b: Float64) -> Bool:
    """True when `a` is an exact, nonzero integral multiple of `b`."""
    if b == 0 or not _is_finite(a) or not _is_finite(b):
        return False
    var q = a / b
    return _is_finite(q) and q != 0 and q == trunc(q)


@always_inline
def _is_finite(x: Float64) -> Bool:
    # NaN and +/-inf are the only values for which x - x is not 0.
    return x - x == 0


def _covers(a: Optional[String], b: Optional[String]) -> Bool:
    """`a` is at least as strict as `b` for an opaque string constraint."""
    if not b:
        return True
    return Bool(a) and a.value() == b.value()


def _opt_eq(a: Optional[Float64], b: Optional[Float64]) -> Bool:
    if not a or not b:
        return not a and not b
    return a.value() == b.value()


def _max_opt(a: Optional[Int], b: Optional[Int]) -> Optional[Int]:
    if not a:
        return b
    if not b:
        return a
    return max(a.value(), b.value())


def _min_opt(a: Optional[Int], b: Optional[Int]) -> Optional[Int]:
    if not a:
        return b
    if not b:
        return a
    return min(a.value(), b.value())
