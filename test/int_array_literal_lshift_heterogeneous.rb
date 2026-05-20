# `<int_array literal> << <non-int>` shape (`[1, 2] << "c"`).
# Pushing a non-int element onto a literal int_array previously
# emitted sp_IntArray_push with a pointer-from-int-conversion
# warning and produced a broken array. The contextual lift
# rebuilds the recv as a poly_array of box_int(...) elements and
# pushes the boxed arg, matching CRuby's heterogeneous-array
# semantic.
#
# Scoped to direct ArrayNode literals on the LHS, so a stored
# ivar / local (`@slots = [nil] * N; @slots << ...`) keeps the
# existing observation-widening path. Issue #619 puzzle 7.
p ([1, 2] << "c") == [1, 2, "c"]       # true
p ([1, 2] << nil) == [1, 2, nil]       # true
p ([1, 2] << 3) == [1, 2, 3]           # true (int <<, no lift)
