# .__send__(:sym, args) and .__send__("sym", args) statically rewrite
# to .sym(args) at parse time, identical to .send. CRuby exposes
# __send__ as the overrides-resistant alias of send; Spinel does not
# model the visibility distinction, so the two are semantically
# equivalent here.

class Calc
  def add(a, b)
    a + b
  end

  def hello
    "hi"
  end
end

c = Calc.new

# Symbol arm: .__send__(:sym, args) -> .sym(args).
puts c.__send__(:add, 10, 20)   # 30
puts c.__send__(:hello)         # hi

# String arm: .__send__("sym", args) -> .sym(args).
puts c.__send__("add", 3, 4)    # 7
puts c.__send__("hello")        # hi

puts "done"
