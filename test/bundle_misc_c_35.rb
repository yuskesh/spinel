# Bundled tests:
#   - array_mul_string
#   - for_multi_var
#   - symbol_inspect_spl_marker
#   - split_no_arg_ws
#   - scalar_zero_truthiness
#   - reduce_sym_op
#   - array_of_hash_lit
#   - integer_div

# === array_mul_string ===
def t_array_mul_string
  # `Array * sep` (String arg) is equivalent to Array#join(sep) and
  # returns a string. Previously spinel routed it through the int-repeat
  # path which treated the string pointer as a loop count.
  puts ([1, 2, 3] * ",")
  puts ([1, 2, 3] * ",").inspect
  puts ([1, 2, 3] * "")
  puts (["a", "b", "c"] * "-")
end
t_array_mul_string

# === for_multi_var ===
def t_for_multi_var
  # `for a, b in coll` destructures each element into multiple LVs.
  for a, b in [[1, 2], [3, 4]]
    puts "#{a}, #{b}"
  end

  for x, y, z in [[1, 2, 3], [4, 5, 6]]
    puts "#{x},#{y},#{z}"
  end
end
t_for_multi_var

# === symbol_inspect_spl_marker ===
def t_symbol_inspect_spl_marker
  # Symbol#inspect literal prefix must use SPL marker so sp_str_byte_len's
  # s[-1] marker check is well-defined (avoids -O2 __builtin_trap).
  puts :hello.inspect          # :hello

  sym = :world
  puts sym.inspect             # :world

  puts "done"
end
t_symbol_inspect_spl_marker

# === split_no_arg_ws ===
def t_split_no_arg_ws
  # #507. `s.split` (no arg) used to emit `sp_str_split(s, 0)` and
  # segfault at strlen(NULL) -- new sp_str_split_ws helper matches
  # Ruby's whitespace-mode split (no arg / nil arg form).
  p "1 2 3".split
  p "  leading  trailing  ".split
  p "a\tb\nc".split          # mixed whitespace
  p "".split                  # empty
  p "abc".split(nil)          # explicit nil arg -> same as no-arg
  p "a,b,c".split(",")        # non-whitespace sep (regression guard)
end
t_split_no_arg_ws

# === scalar_zero_truthiness ===
def t_scalar_zero_truthiness
  idx = 0
  if idx
    puts "local zero truthy"
  else
    puts "local zero falsey"
  end

  lookup = { "zero" => 0 }
  value = lookup["zero"]
  if value
    puts "hash zero truthy"
  else
    puts "hash zero falsey"
  end

  width = 0 || 800
  puts width
end
t_scalar_zero_truthiness

# === reduce_sym_op ===
def t_reduce_sym_op
  # #506. `[1,2,3,4].reduce(:*)` previously fell through unresolved
  # on int_array. Codegen now folds the symbol-arg form inline for
  # binary arithmetic / bitwise ops on int_array.
  p [1, 2, 3, 4].reduce(:*)    # 24
  p [1, 2, 3, 4].reduce(:+)    # 10
  p [10, 1, 2].reduce(:-)      # 7
  p [5, 3].inject(:%)          # 2
  p [12, 10].inject(:&)        # 8
  p [5, 3].inject(:|)          # 7
  p [5, 3].inject(:^)          # 6
  p [42].reduce(:+)            # 42 (single-element)
  p [].reduce(:+)              # nil (empty seedless fold, matching CRuby)
end
t_reduce_sym_op

# === array_of_hash_lit ===
def t_array_of_hash_lit
  # Issue #402: array literal of hash literals (`[{n: 3}, {n: 1}]`)
  # was typed as `sp_IntArray *` (the bottom default), but the
  # elements were emitted as `sp_SymIntHash *`. Fix: inference
  # recognises hash-typed elements and returns `poly_array`.
  arr = [{n: 3}, {n: 1}, {n: 2}]
  puts arr.length          # 3
  puts arr[0][:n].to_s     # 3
  puts arr[1][:n].to_s     # 1
  puts arr[2][:n].to_s     # 2

  mixed = [{a: 1}, {b: "x"}]
  puts mixed.length        # 2
end
t_array_of_hash_lit

# === integer_div ===
def t_integer_div
  # basic
  puts 7.div(2)
  puts 10.div(5)

  # exact
  puts 6.div(3)

  # negative -- floor division rounds toward -infinity
  puts((-7).div(2))
  puts 7.div(-2)
  puts((-7).div(-2))

  # zero dividend
  puts 0.div(3)

  # one
  puts 1.div(1)
  puts((-1).div(1))

  # large
  puts 1000001.div(1000)
end
t_integer_div
