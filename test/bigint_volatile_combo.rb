# Regression test: bigint operations inside begin/rescue must compile
# clean under -Werror. Locals in setjmp-bearing functions are emitted
# `volatile sp_Bigint *`, but the bigint runtime takes plain
# `sp_Bigint *`. Without (sp_Bigint *) casts at every emit site, gcc
# / clang warn about discarded qualifiers, and the test runner's
# -Werror promotes that to a hard fail.
#
# Exercises every bigint operator dispatch path that previously
# emitted a cast-free local read. Output values are not the focus —
# the test runner gates on the C code compiling clean with -Werror.

a = 1
i = 0
while i < 50
  a = a * 2
  i = i + 1
end

# Inside begin/rescue: every binary operator + comparison + to_s/to_i
begin
  b = a + a
  puts b.to_s.length > 0
  c = a - a
  puts c.to_s.length > 0
  d = a * a
  puts d.to_s.length > 0
  e = a / 2
  puts e.to_s.length > 0
  f = a % 7
  puts f.to_s.length > 0
  puts a > 0
  puts a < 0
  puts a >= 0
  puts a <= 0
  puts a == a
  puts a != 0
  puts a.to_s.length > 0
  puts a.to_i > 0
rescue
  puts "caught"
end

# Compound assignment inside begin/rescue. `x = x + 1` desugars to
# LocalVariableWriteNode + CallNode; the `+=` family lowers through
# LocalVariableOperatorWriteNode, a distinct code path that has its
# own bigint cast in compile_LocalVariableAndWriteNode.
begin
  x = a
  x += 1
  x *= 2
  x -= 1
  x /= 2
  x %= 7
  puts x.to_s.length > 0
rescue
  puts "caught"
end

# puts/p of bigint inside begin/rescue. Single-arg dispatches through
# compile_puts_single; multi-arg through compile_puts. Both arms have
# their own bigint cast and need exercising. `print` lands in the
# multi-arg path too.
begin
  puts a
  puts a, a
  print a, "\n"
rescue
  puts "caught"
end
