# each_with_index on a POLY receiver - an array reached as a boxed sp_RbVal (a
# def parameter, or an element of a nested array) - went through poly dispatch
# that lacked each_with_index and raised NoMethodError, though plain `each` had a
# poly path (sp_poly_each_elem). It now binds the whole element + the loop index,
# never splatting a nested-array element (unlike `each`).
def show(cells)
  cells.each_with_index do |v, i|
    puts "#{i}:#{v}"
  end
end
show(["x", 1, :sym])

def rows(data)
  data.each do |row|
    row.each_with_index { |c, i| puts "#{i}=#{c}" }
  end
end
rows([["a", "b"], ["c", "d"]])

def total(xs)
  sum = 0
  xs.each_with_index { |_v, i| sum += i }
  sum
end
puts total([10, 20, 30, 40])

# A boxed (sp_RbVal) poly receiver: `cond ? array : hash` gives a TY_POLY value,
# the path this PR adds each_with_index to. The cases below exercise its block
# param binding.
def poly(cond)
  cond ? [10, 20, 30] : { a: 1 }
end

# UNUSED index param: the pruned `i` local is never declared, so the binding
# must be skipped (not emit an assignment to an undeclared `lv_i`).
poly(true).each_with_index { |v, i| puts v }

# EXTRA param beyond the yielded (element, index) arity binds to nil each
# iteration -- a reassigned extra must not leak its prior value across passes.
poly(true).each_with_index { |v, i, x| x = 99 unless x.nil?; puts "#{v}/#{i}/#{x.inspect}" }
