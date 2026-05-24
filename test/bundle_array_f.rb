# Bundled tests:
#   - array_group_by_poly_each_chain
#   - array_of_hash_aset_sym
#   - array_partition_gc
#   - array_plus_cross_type
#   - array_push_mixed_types

# === array_group_by_poly_each_chain ===
def t_array_group_by_poly_each_chain
  # Array#group_by + Hash#each chain on a poly_array of sym_poly_hashes.
  # spinel fuses `<arr>.group_by(blk).each |k, rows|` into a single emit
  # that builds a sp_PolyPolyHash keyed by the block's return value, with
  # each slot holding a sp_PolyArray of elements (boxed as poly). The
  # iteration loop then unboxes the slot back to a typed sp_PolyArray so
  # `rows.map { ... }` / `rows.sum` / `rows.size` reach the existing
  # poly_array dispatch arms.
  #
  # Two downstream gaps the fusion exposes get fixed in the same change:
  # `Array#size` was missing for poly_array (only `.length`), and
  # `Array#sum` on a poly_array now uses a runtime helper that sums the
  # int-tagged elements (the shape the .map produces for `_1[:int_key]`).
  # Float interpolation also routes through sp_float_to_s so 4.0 renders
  # as "4.0" (matching Ruby's Float#to_s) rather than "4" (printf %g).
  
  scores = [
    { shop: "ginza", score: 5 },
    { shop: "ginza", score: 4 },
    { shop: "shibuya", score: 3 },
    { shop: "shibuya", score: 5 }
  ]
  
  scores.group_by { _1[:shop] }.each do |shop, rows|
    avg = rows.map { _1[:score] }.sum.to_f / rows.size
    puts "#{shop}: #{avg}"
  end
end
t_array_group_by_poly_each_chain

# === array_of_hash_aset_sym ===
def t_array_of_hash_aset_sym
  # Chained `[]=` / `[]op=` on a poly_array element that is a
  # boxed SymIntHash: `arr[i][:k] = v` and `arr[i][:k] += v`.
  #
  # Mirror of Issue #456 (chained_aset_on_poly_hash_recv.rb) for
  # the symbol-idx side. The string-idx arm was already present in
  # the poly-recv `[]=` dispatch, but the symbol-idx arm was
  # missing — the assignment fell through to the Array arms
  # (PolyArray / PtrArray / IntArray) whose cls_id never matched
  # SP_BUILTIN_SYM_INT_HASH, so the write silently no-op'd.
  #
  # Same gap existed for `compile_index_op_assign` (`[]op=`): when
  # recv was inferred as poly (e.g. `arr[i]` returning sp_RbVal),
  # no Hash-storage arm was emitted, so `arr[i][:k] += v` and the
  # each-block analog `f[:k] += v` (with `f` widened to poly)
  # silently dropped the modification.
  #
  # Fix: add a symbol-idx arm next to the string-idx arm in the
  # `[]=` dispatch (SymIntHash + SymPolyHash), and a poly-recv +
  # symbol-idx arm to `compile_index_op_assign` (SymIntHash;
  # SymPolyHash compound-assign needs poly-op semantics, deferred).
  
  # T1: direct `[]=` on poly_array of sym_int_hash
  arr1 = [{x: 1}]
  arr1[0][:x] = 99
  puts arr1[0][:x].to_s
  
  # T2: compound `[]+=` on poly_array of sym_int_hash
  arr2 = [{x: 1}]
  arr2[0][:x] += 100
  puts arr2[0][:x].to_s
  
  # T3: `each` block mutating each hash via `f[:k] += v`
  arr3 = []
  arr3 << {x: 1}
  arr3 << {x: 3}
  arr3.each { |f| f[:x] += 100 }
  puts arr3[0][:x].to_s
  puts arr3[1][:x].to_s
  
  # T4: `each_with_index` block doing direct `f[:k] = v`
  arr4 = [{x: 1}, {x: 3}]
  arr4.each_with_index { |f, i| f[:x] = i * 10 }
  puts arr4[0][:x].to_s
  puts arr4[1][:x].to_s
end
t_array_of_hash_aset_sym

# === array_partition_gc ===
def t_array_partition_gc
  # Regression: Array#partition (block form) returns a tuple holding two
  # inner arrays. The tuple's sp_gc_alloc was emitted with scan=NULL, so
  # the inner arrays were swept while the tuple was still alive, and a
  # subsequent allocation reused the freed memory — `parts[0].length`
  # came back as the length of whatever object now sat there.
  
  arr = [1, 2, 3, 4, 5, 6]
  parts = arr.partition { |x| x.odd? }
  
  # Force many GCs by allocating lots of GC-managed objects.
  i = 0
  while i < 200000
    tmp = [1, 2, 3]
    tmp.push(i)
    i += 1
  end
  
  puts parts[0].length   # 3
  puts parts[1].length   # 3
  puts "done"
end
t_array_partition_gc

# === array_plus_cross_type ===
def t_array_plus_cross_type
  # Issue #662: Array#+ concatenating arrays of different element
  # types. Previously the codegen assumed both operands had the same
  # array prefix and used the lhs's *_get / *_length / *_push for
  # both -- failed C compilation with a pointer-type mismatch.
  #
  # Fix: infer_call_type returns "poly_array" when lhs is a typed
  # array and rhs is an array of a different element type. compile_+
  # detects the same shape and builds an sp_PolyArray, boxing each
  # source element via sp_box_* (sp_box_obj with cls_id for ptr).
  
  a1 = [1, 2, 3]
  a2 = ["x", "y", "z"]
  a3 = a1 + a2
  puts a3.length
  puts a3[0]
  puts a3[3]
  puts "ok"
  
  # Float + Int round-trip
  b1 = [1.5, 2.5]
  b2 = [10, 20]
  b3 = b1 + b2
  puts b3.length
  puts b3[0]
  puts b3[2]
  puts "done"
end
t_array_plus_cross_type

# === array_push_mixed_types ===
def t_array_push_mixed_types
  # Issue #663: Array#<< with heterogeneous element types on an
  # initially-empty array. Previously the type inference latched
  # onto the first push's element type (transitioning int_array ->
  # str_array / float_array / sym_array) but didn't widen to
  # poly_array when a subsequent push had a different type. The
  # codegen then emitted e.g. sp_StrArray_push with an int literal
  # and failed C compilation.
  #
  # Fix: scan_locals's push/<< inference now widens to poly_array
  # when the slot is already a non-target concrete array type and
  # the next push is of a different family.
  
  arr = []
  arr << 1
  arr << "string"
  arr << :symbol
  puts arr.length
  puts arr[0]
  puts arr[1]
  puts arr[2]
  puts "ok"
end
t_array_push_mixed_types

