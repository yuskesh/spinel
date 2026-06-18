# a Range value is always truthy in condition position
def range_truthy(r) = r ? "yes" : "no"
puts range_truthy(1..3)

c = (1..5)
puts "range kept" if c

# a Class value is always truthy
def class_truthy(k) = k ? "has class" : "no class"
puts class_truthy(String)

# guard pattern: collect when the (truthy) value is present
out = []
val = (1..2)
out << "kept" if val
p out

# other always-truthy concrete types in condition position. Each value flows
# through a monomorphic helper param so it keeps its concrete static type (and
# is not constant-folded), exercising the per-type truthiness branches.
def time_truthy(t) = t ? "yes" : "no"
puts time_truthy(Time.at(0))            # value type (sp_Time)

def regex_truthy(re) = re ? "yes" : "no"
puts regex_truthy(/x/)                  # pointer type (mrb_regexp_pattern *)

def method_truthy(m) = m ? "yes" : "no"
puts method_truthy(method(:range_truthy))  # pointer type (sp_BoundMethod *)

def random_truthy(r) = r ? "yes" : "no"
puts random_truthy(Random.new(42))      # pointer type (sp_Random *)

def curry_truthy(p) = p ? "yes" : "no"
puts curry_truthy(->(a, b) { a + b }.curry)  # pointer type (sp_Curry *)
