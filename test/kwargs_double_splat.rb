# `foo(**h)` — double-splat a sym-keyed hash into the callee's
# keyword params. Only the single-arg shape is wired here
# (no positional + splat mixing); the splat hash must be
# sym-keyed (sym_int/sym_str/sym_poly).
def foo(a:, b:)
  puts "a=#{a} b=#{b}"
end
h = {a: 1, b: 2}
foo(**h)

def bar(x:, y:, z:)
  puts "#{x},#{y},#{z}"
end
h2 = {x: 10, y: 20, z: 30}
bar(**h2)
