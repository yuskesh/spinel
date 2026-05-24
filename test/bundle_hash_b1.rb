# Bundled tests:
#   - hash_dup_str_int_str
#   - hash_each_block_param_types
#   - hash_merge_cross_variant

# === hash_dup_str_int_str ===
def t_hash_dup_str_int_str
  # Hash#dup was missing from the str_str_hash and int_str_hash codegen
  # dispatch tables -- the runtime helpers exist (sp_StrStrHash_dup,
  # sp_IntStrHash_dup) but the codegen arms didn't dispatch to them, so
  # `m = h.dup` lowered to `cannot resolve call to 'dup' on str_str_hash
  # (emitting 0)` and subsequent `.length` / `[]=` / `.each` calls on
  # the int-typed `0` segfaulted at runtime.
  #
  # Sibling Hash variants (sym_int / sym_str / sym_poly / str_int /
  # str_poly) already had dup arms; this closes the coverage gap.
  # Issue #592.
  
  # str_str_hash
  h1 = {"a" => "1", "b" => "2"}
  m1 = h1.dup
  m1["c"] = "3"
  puts h1.length          # 2
  puts m1.length          # 3
  puts m1["a"]            # 1
  puts m1["c"]            # 3
  
  # int_str_hash
  h2 = {1 => "one", 2 => "two"}
  m2 = h2.dup
  m2[3] = "three"
  puts h2.length          # 2
  puts m2.length          # 3
  puts m2[1]              # one
  puts m2[3]              # three
  
  # Verify the dup is independent (original unchanged).
  puts h1.has_key?("c")   # false
  puts h2.has_key?(3)     # false
end
t_hash_dup_str_int_str

# === hash_each_block_param_types ===
def t_hash_each_block_param_types
  # `h.each do |k, v|` previously had its block params typed via
  # elem_type_of_array(recv_type), which only knows about typed
  # arrays — for a hash receiver it fell back to "int" for both
  # `k` and `v`. Code inside the block like `puts k + ": " + v.to_s`
  # then had analyze cache (and codegen's infer_type fallback)
  # return "int" for the chained `+`, lowering `puts` to
  # `printf("%lld", (long long)(string_concat + int_to_s))` — both
  # the cast and the raw `+` between pointers fail C compile.
  #
  # Fix: block_param_type_at now branches on is_hash_type for the
  # `each` family and returns the hash's key part for pi==0 and the
  # value part for pi==1, expanded to full names (str→string,
  # sym→symbol). Codegen's infer_type CallNode cache-miss path also
  # learned `+` on string recv returns string and `.to_s` returns
  # string, covering nodes inside block bodies that walk_and_cache
  # doesn't cache.
  
  h = { "a" => 1, "b" => 2 }
  h.each do |k, v|
    puts k + ": " + v.to_s
  end
end
t_hash_each_block_param_types

# === hash_merge_cross_variant ===
def t_hash_merge_cross_variant
  # Issue #661: Hash#merge across hash variants with different value
  # types. Previously the codegen used the receiver's prefix
  # unconditionally (sp_SymIntHash_merge(h1, h2) where h2 was
  # sp_SymStrHash *), failing C compile.
  #
  # Fix: when recv and arg are both sym-keyed (or both str-keyed) but
  # their value types differ, the analyzer pulls the call's return
  # type up to sym_poly_hash / str_poly_hash, and codegen promotes
  # both sides via the existing *_to_sym_poly / *_from_str_int_hash
  # converters before dispatching sp_*PolyHash_merge.
  
  # Sym + Sym with mixed value types.
  h1 = { a: 1, b: 2 }
  h2 = { c: "x", d: "y" }
  h3 = h1.merge(h2)
  puts h3[:a].inspect
  puts h3[:c].inspect
  
  # Str + Str with mixed value types.
  s1 = { "a" => 1, "b" => 2 }
  s2 = { "c" => "x", "d" => "y" }
  s3 = s1.merge(s2)
  puts s3["a"].inspect
  puts s3["c"].inspect
  
  puts "ok"
end
t_hash_merge_cross_variant

