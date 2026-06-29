# A poly value that holds a builtin array must still append on push/<</append
# even when a user class defines push/<<: the poly-dispatch switch needs a
# builtin-array arm (sp_poly_shl), or the append is silently dropped into an
# empty switch. The user class's own push still dispatches for object receivers.
class Stack
  def push(x)
    x
  end
end

def pick(flag)
  flag ? [1, 2] : ["a"]   # mixed element kinds -> poly array (poly receiver)
end

arr = pick(true)
arr.push(99)
arr << 100
arr.append(101)
puts arr.length
puts arr.inspect

puts Stack.new.push(7)    # the user push still works on its own object
