# Bundled tests:
#   - array_at_append_string_chomp_bang
#   - array_eq_int_vs_sym_literal
#   - array_index_nil_for_not_found
#   - array_keyed_hash_int_array_eql
#   - array_literal_all_nil_first_is_nil
#   - array_new_n_fills_nil
#   - array_new_nil_fill
#   - array_sum_init

# === array_at_append_string_chomp_bang ===
def t_array_at_append_string_chomp_bang
  # Method aliases / shapes from #619 puzzles 8 / 9 / 10:
  #   Array#at(n)      -> Array#[n]
  #   Array#append(x)  -> Array#push(x), returning self (the array)
  #   String#chomp!    -> String#chomp (matches the changed-case;
  #                       the Ruby `nil if no change` mutator semantic
  #                       isn't preserved since spinel strings are
  #                       immutable)
  # Pre-fix all three lowered to the unresolved-call fallback "emit 0"
  # and the comparison surfaces returned false.
  
  p([1, 2, 3].at(1) == 2)
  p(%w[a].append("b") == %w[a b])
  p("ab\n".chomp! == "ab")
end
t_array_at_append_string_chomp_bang

# === array_eq_int_vs_sym_literal ===
def t_array_eq_int_vs_sym_literal
  # `[0,0,0] == %i[a a a]` must answer FALSE -- CRuby compares element
  # classes too, so int 0 != sym :a even when their encoded ids match.
  # Spinel's sym_array storage shares the sp_IntArray layout with
  # int_array (sym ids stored as raw mrb_int), and sp_IntArray_eq
  # compares raw bytes. With SPS_a == 0, both arrays' bytes are
  # [0,0,0] and the eq returned `true`, breaking `!=` to false.
  #
  # Fix: when the equality is between literal `[...]` and literal
  # `%i[...]` and the static types differ (int_array vs sym_array),
  # short-circuit to FALSE. The `<<`-widened "int_array carrying
  # syms at runtime" shape from issue #555 keeps both sides typed
  # `int_array` (the analyzer leaves the static type alone), so the
  # guard's "different static types" condition doesn't fire and
  # IntArray_eq continues to apply. Issue #600 puzzle 1.
  
  p [0, 0, 0] != %i[a a a]                 # true
  p [0, 0, 0] == %i[a a a]                 # false
  p %i[a a a] != [0, 0, 0]                 # true
  
  # Sibling shapes -- arrays of the same element-kind still compare
  # correctly via IntArray_eq.
  p [1, 2, 3] == [1, 2, 3]                 # true
  p %i[a b c] == %i[a b c]                 # true
  p %i[a b c] != %i[a b]                   # true (length differs)
  
  # Issue #555 test #6 shape: int_array with sym pushes at runtime.
  # Both sides resolve to lt == at == "int_array" statically, so the
  # new guard doesn't engage and IntArray_eq compares element ids
  # (which match for the :foo pushed and the :foo literal in [:foo]).
  a = [1]
  a.shift
  a << :foo
  p a == [:foo]                            # true
end
t_array_eq_int_vs_sym_literal

# === array_index_nil_for_not_found ===
def t_array_index_nil_for_not_found
  # `Array#index(x)` / `#find_index(x)` / `#rindex(x)` return
  # Integer | nil in CRuby (nil when not found). spinel previously
  # returned the raw -1 sentinel from `sp_*Array_index`, which
  # diverged from CRuby's nil for the not-found case.
  #
  # spinel positions itself as a Ruby SUBSET, so documented Ruby
  # APIs must match CRuby behavior. The fix: codegen now routes
  # Array#index family through `sp_*Array_index_poly` wrappers
  # that box nil for not-found / box int for found. The type
  # inference returns "poly" for the call result so `.nil?` /
  # `== nil` checks dispatch through the standard poly-tag path.
  #
  # The raw `_index` helpers (returning -1) stay for any internal
  # caller that needs the sentinel.
  
  # Int arrays
  ints = [10, 20, 30, 40]
  puts ints.index(20).inspect       # 1
  puts ints.index(999).inspect      # nil
  puts ints.index(999).nil?         # true
  puts ints.index(20).nil?          # false
  puts ints.find_index(30).inspect  # 2
  
  # String arrays
  strs = ["alpha", "beta", "gamma"]
  puts strs.index("beta").inspect   # 1
  puts strs.index("zeta").inspect   # nil
  puts strs.rindex("alpha").inspect # 0
  puts strs.rindex("zeta").inspect  # nil
end
t_array_index_nil_for_not_found

# === array_keyed_hash_int_array_eql ===
def t_array_keyed_hash_int_array_eql
  # Array-keyed Hash. `entries[[a, b]] ||= ...` was broken because
  # spinel's PolyPolyHash defaulted to pointer-identity comparison
  # for SP_TAG_OBJ keys — every fresh `[a, b]` literal allocated a
  # new IntArray, so identical-content keys never matched and the
  # `||=` never deduped. The cache grew unboundedly and reads with
  # a fresh `[a, b]` returned nil.
  #
  # Fix: extend the codegen-emitted sp_obj_hash_hook /
  # sp_obj_eql_hook to handle SP_BUILTIN_INT_ARRAY with element-wise
  # content hash + comparison. Two IntArray instances with the same
  # elements now hash and eql? identically — array-keyed Hash
  # behaves like CRuby's `Hash` with the [a,b]-style key idiom.
  
  entries = {}
  entries[[1, 2]] = "a"
  entries[[3, 4]] = "b"
  entries[[1, 2]] ||= "z"   # already set, no overwrite
  puts entries[[1, 2]]      # "a"
  puts entries[[3, 4]]      # "b"
  puts entries.length       # 2
  
  # Heterogeneous keys still work — int / IntArray / string mixed.
  mixed = {}
  mixed[42] = "int-key"
  mixed[[5, 6, 7]] = "arr-key"
  mixed[[5, 6, 7]] ||= "no-overwrite"
  puts mixed[42]            # int-key
  puts mixed[[5, 6, 7]]     # arr-key
  puts mixed.length         # 2
end
t_array_keyed_hash_int_array_eql

# === array_literal_all_nil_first_is_nil ===
def t_array_literal_all_nil_first_is_nil
  # `[nil, nil, ...].first` / `.last` returns nil. Pre-fix the
  # array-literal lowering pushed each NilNode as `sp_IntArray_push(_t, 0)`
  # (the int_array default) and `.first` returned int 0, so `.nil?`
  # folded to false. The peephole keeps the int_array layout for the
  # rest of the inference -- lifting all-nil literals to poly_array
  # breaks optcarrot's `[nil] * N` initialization -- but recognises
  # the `[nil, ...].first / .last` shape at the analyzer's CallNode
  # return-type site. Issue #619 puzzle 5.
  p [nil].first.nil?               # true
  p [nil].last.nil?                # true
  p [nil, nil, nil].first.nil?     # true
  p [nil, nil, nil].last.nil?      # true
end
t_array_literal_all_nil_first_is_nil

# === array_new_n_fills_nil ===
def t_array_new_n_fills_nil
  # `Array.new(n)` (single arg, no fill value, no block) is CRuby
  # shorthand for `Array.new(n, nil)`: an array of n nils, NOT an
  # empty array. Pre-fix the single-arg lowering returned
  # `sp_IntArray_new()` (length 0) and downstream `.length` /
  # `.inspect` / `[i].nil?` saw the wrong shape. Issue #619 puzzle 4.
  puts Array.new(3).length         # 3
  puts Array.new(3).inspect        # [nil, nil, nil]
  puts Array.new(3)[0].nil?        # true
  puts Array.new(0).length         # 0
  puts Array.new(5).length         # 5
end
t_array_new_n_fills_nil

# === array_new_nil_fill ===
def t_array_new_nil_fill
  # `Array.new(n, nil)` -- the fill value is the nil singleton, so
  # MRI fills each slot with `nil` and `.inspect` prints
  # "[nil, nil, ...]". Spinel previously lowered the call to an
  # int_array filled with the C default (0), and inspect printed
  # "[0, 0, 0]". Lowering must produce a sp_PolyArray so the slot
  # can carry the nil tag and `.inspect` / `[i].nil?` / etc. see
  # the actual nil.
  
  ary = Array.new(3, nil)
  puts ary.inspect
  puts ary[0].nil?
  puts ary[1].nil?
  puts ary[2].nil?
  puts ary.length
end
t_array_new_nil_fill

# === array_sum_init ===
def t_array_sum_init
  # Array#sum's init argument was silently dropped on the IntArray /
  # FloatArray dispatch paths -- spinel emitted sp_IntArray_sum(rc)
  # (no init parameter) and the block-form accumulator in
  # compile_array_sum_block was hardcoded to `mrb_int t = 0;`.
  # Pre-fix:
  #   [1,2,3].sum(10)             # => 6   (CRuby: 16)
  #   [].sum(7)                   # => 0   (CRuby: 7)
  #   [1,2,3].sum(10) { |x| x*2 } # => 12  (CRuby: 22)
  #
  # Fix: sp_IntArray_sum / sp_FloatArray_sum take an init parameter
  # and seed the accumulator with it; codegen forwards
  # compile_arg0(nid) at every sum dispatch. compile_array_sum_block
  # initialises tmp_sum from compile_arg0(nid).
  
  # IntArray, no block
  puts [1, 2, 3].sum(10)            # 16
  puts [1, 2, 3].sum                # 6   (no-init regression check)
  puts [].sum(7)                    # 7   (empty + init survives)
  puts [].sum                       # 0   (empty default still 0)
  puts [1, 2].sum(-5)               # -2  (negative init)
  
  # IntArray, with block
  puts [1, 2, 3].sum(10) { |x| x * 2 }   # 22
  puts [1, 2, 3].sum { |x| x * 2 }       # 12  (block, no init)
  puts [].sum(7) { |x| x * 2 }           # 7   (empty + init w/ block)
  
  # FloatArray, no block
  puts [1.5, 2.5].sum(0.5)          # 4.5
  puts [1.5, 2.5].sum               # 4.0 (no-init regression check)
  puts [1.5, 2.5].sum(1)            # 5.0 (int init implicitly widens)
  puts [1.5, 2.5].sum(-0.5)         # 3.5 (negative float init)
  
  # Poly init: a heterogeneous-array element resolves to a poly local.
  # compile_arg0_as_int / compile_arg0_as_float unbox via `.v.i` / `.v.f`;
  # without them gcc rejects passing sp_RbVal to mrb_int / mrb_float.
  poly_arr = [10, "x"]
  init_p = poly_arr[0]
  puts [1, 2].sum(init_p)           # 13 (poly init unboxed as mrb_int)
  
  fpoly_arr = [1.5, "x"]
  finit_p = fpoly_arr[0]
  puts [1.0, 2.0].sum(finit_p)      # 4.5 (poly init unboxed as mrb_float)
  
  # Tag-mixed: poly is INT-tagged but the FloatArray expects mrb_float.
  # sp_poly_to_f dispatches on the tag so the int value is coerced to
  # float rather than reinterpreted bit-for-bit from `.v.f`.
  ipoly_for_float = [1, "x"][0]
  puts [1.0, 2.0].sum(ipoly_for_float)   # 4.0 (poly INT->float via sp_poly_to_f)
  
  # FloatArray + block + float init: compile_array_sum_block now picks
  # an mrb_float accumulator and compile_arg0_as_float when recv_type is
  # float_array, so the 0.5 seed and the float block result are no
  # longer truncated. Use distinct block-param names (`fx` / `fy`) so
  # they don't get widened by the earlier int blocks' `|x|` -- spinel
  # hoists block params to a shared function-scope local, an orthogonal
  # limitation outside this PR's scope.
  puts [1.5, 2.5].sum(0.5) { |fx| fx }      # 4.5
  puts [1.0, 2.0].sum { |fy| fy * 1.5 }     # 4.5
  
  puts "done"
end
t_array_sum_init

