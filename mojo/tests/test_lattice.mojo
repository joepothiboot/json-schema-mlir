# Tests for the Mojo constraint lattices.
#
# The first group mirrors test/Dialect/Schema/schema-canonicalize.mlir case by
# case. The second checks the property the canonicalizer relies on, over a
# grid of constraints and values:
#
#   meet(a, b) exists  =>  meet(a, b).validate(x) == a.validate(x) and b.validate(x)
#   a.subsumes(b)      =>  meet(a, b) == a
#
# Run from the repo root:  pixi run test-mojo

from std.testing import assert_equal, assert_false, assert_raises, assert_true

from schema import NumberConstraints, StringConstraints, code_point_length


# --- cases from schema-canonicalize.mlir -------------------------------------


def test_redundant_min_length() raises:
    var strong = StringConstraints().with_min_length(5)
    var weak = StringConstraints().with_min_length(2)
    assert_true(strong.subsumes(weak))
    assert_false(weak.subsumes(strong))
    assert_true(StringConstraints.meet(strong, weak).value() == strong)


def test_fuse_constraint_tree() raises:
    var a = StringConstraints().with_min_length(5)
    var b = StringConstraints().with_max_length(32)
    var c = StringConstraints().with_min_length(2).with_pattern("^[a-z]+$")
    var ab = StringConstraints.meet(a, b).value().copy()
    var abc = StringConstraints.meet(ab, c).value().copy()
    var expected = (
        StringConstraints()
        .with_min_length(5)
        .with_max_length(32)
        .with_pattern("^[a-z]+$")
    )
    assert_true(abc == expected)


def test_redundant_numeric_bounds() raises:
    var strong = NumberConstraints().with_minimum(10).with_integral()
    var weak = NumberConstraints().with_minimum(0)
    assert_true(strong.subsumes(weak))
    assert_true(NumberConstraints.meet(strong, weak).value() == strong)


def test_exclusive_bound_subsumes_inclusive() raises:
    var excl = NumberConstraints().with_minimum(3, exclusive=True)
    var incl = NumberConstraints().with_minimum(3)
    assert_true(excl.subsumes(incl))
    assert_false(incl.subsumes(excl))


def test_distinct_patterns_have_no_meet() raises:
    var a = StringConstraints().with_pattern("^a")
    var b = StringConstraints().with_pattern("^b")
    assert_false(Bool(StringConstraints.meet(a, b)))


def test_multiple_of_meet() raises:
    var two = NumberConstraints().with_multiple_of(2)
    var four = NumberConstraints().with_multiple_of(4)
    var three = NumberConstraints().with_multiple_of(3)
    assert_true(NumberConstraints.meet(two, four).value() == four)
    assert_false(Bool(NumberConstraints.meet(two, three)))


def test_person_example() raises:
    # examples/person: the three allOf branches on `age` fuse into one.
    var age_int = NumberConstraints().with_minimum(0).with_integral()
    var age_upper = NumberConstraints().with_maximum(150, exclusive=True)
    var age_adult = NumberConstraints().with_minimum(18)
    var fused = NumberConstraints.meet(
        NumberConstraints.meet(age_int, age_upper).value(), age_adult
    ).value().copy()
    var expected = (
        NumberConstraints()
        .with_minimum(18)
        .with_maximum(150, exclusive=True)
        .with_integral()
    )
    assert_true(fused == expected)
    for age in [0, 17, 18, 42, 149, 150]:
        var x = Float64(age)
        assert_equal(
            fused.validate(x),
            age_int.validate(x) and age_upper.validate(x) and age_adult.validate(x),
        )


# --- validation semantics (as lowered by --lower-schema-to-std) --------------


def test_number_validate() raises:
    var c = (
        NumberConstraints()
        .with_minimum(0)
        .with_maximum(150, exclusive=True)
        .with_integral()
    )
    assert_true(c.validate(0))
    assert_true(c.validate(149))
    assert_false(c.validate(150))
    assert_false(c.validate(-1))
    assert_false(c.validate(2.5))
    var zero: Float64 = 0
    assert_false(c.validate(zero / zero), msg="NaN must fail an ordered bound")


def test_string_lengths_count_code_points() raises:
    assert_equal(code_point_length(""), 0)
    assert_equal(code_point_length("héllo"), 5)  # 6 bytes
    assert_equal(code_point_length("日本語"), 3)  # 9 bytes
    var c = StringConstraints().with_max_length(3)
    assert_true(c.validate("日本語"))
    assert_false(c.validate("héllo"))


def test_pattern_needs_runtime() raises:
    with assert_raises(contains="runtime"):
        _ = StringConstraints().with_pattern("^x").validate("x")


# --- soundness of meet over a grid ------------------------------------------


def number_grid() raises -> List[NumberConstraints]:
    var grid = List[NumberConstraints]()
    for lo in range(4):
        for hi in range(4):
            for m in range(5):
                for integral in range(2):
                    var c = NumberConstraints()
                    if lo == 1:
                        c = c.with_minimum(0)
                    elif lo == 2:
                        c = c.with_minimum(0, exclusive=True)
                    elif lo == 3:
                        c = c.with_minimum(2.5)
                    if hi == 1:
                        c = c.with_maximum(3)
                    elif hi == 2:
                        c = c.with_maximum(3, exclusive=True)
                    elif hi == 3:
                        c = c.with_maximum(10)
                    if m == 1:
                        c = c.with_multiple_of(0.5)
                    elif m == 2:
                        c = c.with_multiple_of(2)
                    elif m == 3:
                        c = c.with_multiple_of(3)
                    elif m == 4:
                        c = c.with_multiple_of(4)
                    if integral == 1:
                        c = c.with_integral()
                    grid.append(c^)
    return grid^


def test_number_meet_is_sound() raises:
    var grid = number_grid()
    var zero: Float64 = 0
    var values: List[Float64] = [
        -2, -0.5, 0, 0.5, 1, 2, 2.5, 3, 4, 6, 10, 12, 1e300,
    ]
    values.append(zero / zero)
    values.append(1 / zero)
    var checked = 0
    for a in grid:
        for b in grid:
            var m = NumberConstraints.meet(a, b)
            if a.subsumes(b):
                assert_true(Bool(m) and m.value() == a, msg="subsumes => meet == a")
            if not m:
                continue
            for x in values:
                assert_equal(
                    m.value().validate(x),
                    a.validate(x) and b.validate(x),
                    msg="meet changed the verdict for x = " + String(x),
                )
                checked += 1
    assert_true(checked > 0)


def test_string_meet_is_sound() raises:
    var grid = List[StringConstraints]()
    var mins: List[Int] = [-1, 0, 2, 5]
    var maxs: List[Int] = [-1, 1, 3, 8]
    for lo in mins:
        for hi in maxs:
            var c = StringConstraints()
            if lo >= 0:
                c = c.with_min_length(lo)
            if hi >= 0:
                c = c.with_max_length(hi)
            grid.append(c^)
    var strings: List[String] = ["", "a", "ab", "héllo", "日本語", "abcdefghij"]
    for a in grid:
        for b in grid:
            var m = StringConstraints.meet(a, b).value().copy()
            for s in strings:
                assert_equal(m.validate(s), a.validate(s) and b.validate(s))


def main() raises:
    test_redundant_min_length()
    test_fuse_constraint_tree()
    test_redundant_numeric_bounds()
    test_exclusive_bound_subsumes_inclusive()
    test_distinct_patterns_have_no_meet()
    test_multiple_of_meet()
    test_person_example()
    test_number_validate()
    test_string_lengths_count_code_points()
    test_pattern_needs_runtime()
    test_number_meet_is_sound()
    test_string_meet_is_sound()
    print("schema: 12 tests passed")
