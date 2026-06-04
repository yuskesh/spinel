# any?/all?/none?/one? with NO block on value-typed arrays. Elements are never
# nil/false, so all? is always true, any? is non-empty, none? is empty, one? is
# a single element. Covers symbol, int, and string arrays.
p(%i[a b c].all?)
p(%i[a b c].any?)
p(%i[a b c].none?)
p(%i[a b c].one?)
p(%i[a].one?)
p([1, 2, 3].all?)
p([].all?)
p([].any?)
p([].none?)
p(["x", "y"].all?)
p(["x"].one?)
