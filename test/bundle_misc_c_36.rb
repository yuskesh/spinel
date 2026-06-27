# Bundled tests:
#   - float_array_sort
#   - integer_literal_large
#   - program_name_gvar
#   - multi_write_as_expression
#   - range_predicate_block
#   - fetch_nil_default_distinguishes_zero
#   - stdlib
#   - math_fn_parity

# === float_array_sort ===
def t_float_array_sort
  a = [3.5, 1.25, 2.0, 0.75, 4.125]
  b = a.sort
  puts b.length

  i = 0
  while i < b.length
    puts b[i]
    i = i + 1
  end

  # Source unchanged.
  puts a[0]
  puts a[1]
end
t_float_array_sort

# === integer_literal_large ===
def t_integer_literal_large
  mod = 4_294_967_296
  puts mod
  puts 16_777_621 % mod

  hash = 2_166_136_261
  data = "spinel"
  i = 0
  while i < data.length
    hash = (hash * 16_777_619 + data.getbyte(i)) % mod
    i += 1
  end

  puts hash
end
t_integer_literal_large

# === program_name_gvar ===
def t_program_name_gvar
  # `$PROGRAM_NAME` / `$0` are Ruby's standard aliases.
  puts $PROGRAM_NAME.length > 0
  puts $0.length > 0
end
t_program_name_gvar

# === multi_write_as_expression ===
def t_multi_write_as_expression
  # #554. `(a, b = 10, 11)` used as an expression returns the RHS array.
  p( (a, b = 10, 11)[1] )
  p a
  p b

  arr = (x, y = "hi", "lo")
  p arr[0]
  p arr[1]
  p x
  p y
end
t_multi_write_as_expression

# === range_predicate_block ===
def t_range_predicate_block
  # (a..b).any?/all?/none?/one? { |i| ... }
  puts (0..5).all? {|i| i >= 0 }      # true
  puts (0..5).all? {|i| i < 3 }       # false
  puts (0..3).all? {|i| (0..3).cover?(i) }  # true

  puts (0..5).any? {|i| i == 3 }      # true
  puts (0..5).any? {|i| i > 100 }     # false

  puts (0..5).none? {|i| i > 100 }    # true
  puts (0..5).none? {|i| i == 3 }     # false

  puts (0..5).one? {|i| i == 3 }      # true
  puts (0..5).one? {|i| i >= 4 }      # false
  puts (0..5).one? {|i| i > 100 }     # false
end
t_range_predicate_block

# === fetch_nil_default_distinguishes_zero ===
def t_fetch_nil_default_distinguishes_zero
  # #682: hash.fetch(k, nil).nil? distinguishes missing-key from a 0 value.
  h0 = {k: 0}
  v0 = h0.fetch(:k, nil)
  puts v0.nil? ? "missing" : "found-0"

  hm = {}
  vm = hm.fetch(:k, nil)
  puts vm.nil? ? "missing" : "found-m"

  h1 = {k: 42}
  v1 = h1.fetch(:k, nil)
  puts v1.nil? ? "missing" : "found-#{v1}"

  hs = {"a" => 0}
  vs = hs.fetch("a", nil)
  puts vs.nil? ? "missing-str" : "found-str-#{vs}"

  hsm = {}
  vsm = hsm.fetch("a", nil)
  puts vsm.nil? ? "missing-str-key" : "found-str-bad"
end
t_fetch_nil_default_distinguishes_zero

# === stdlib ===
def t_stdlib
  # Array#join
  arr = (1..5).to_a
  puts arr.join(", ")

  # Array#uniq
  nums = (1..5).to_a
  nums.push(3)
  nums.push(1)
  uniq = nums.uniq
  puts uniq.length

  # srand/rand
  srand(0)
  r = rand(100)
  puts r >= 0

  # ARGV.length
  puts ARGV.length

  puts "done"
end
t_stdlib

# === math_fn_parity ===
def t_math_fn_parity
  # End-to-end exercise of every recognized Math fn.
  puts Math.sqrt(16.0).to_i.to_s
  puts Math.cos(0.0).to_i.to_s
  puts Math.sin(0.0).to_i.to_s
  puts Math.tan(0.0).to_i.to_s
  puts Math.acos(1.0).to_i.to_s
  puts Math.asin(0.0).to_i.to_s
  puts Math.atan(0.0).to_i.to_s
  puts Math.sinh(0.0).to_i.to_s
  puts Math.cosh(0.0).to_i.to_s
  puts Math.tanh(0.0).to_i.to_s
  puts Math.asinh(0.0).to_i.to_s
  puts Math.acosh(1.0).to_i.to_s
  puts Math.atanh(0.0).to_i.to_s
  puts Math.log(1.0).to_i.to_s
  puts Math.log2(1.0).to_i.to_s
  puts Math.log10(1.0).to_i.to_s
  puts Math.exp(0.0).to_i.to_s
  puts Math.atan2(0.0, 1.0).to_i.to_s
  puts Math.hypot(3.0, 4.0).to_i.to_s
end
t_math_fn_parity
