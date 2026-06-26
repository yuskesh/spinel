# A while/until loop used in value position evaluates to nil (CRuby semantics).
# (A valued `break` would change this, but that is a separate gap and is
# rejected at compile time rather than silently yielding nil.)

# while as an assignment RHS
i = 0
r = while i < 3; i += 1; end
p r                 # nil
p i                 # 3 (the loop ran)

# until in parentheses as an RHS
done = false
n = 0
x = (until done; n += 1; done = n >= 2; end)
p x                 # nil
p n                 # 2

# loop value embedded in an array literal
j = 0
a = [1, (while j < 2; j += 1; end), 2]
p a                 # [1, nil, 2]

# loop value passed as a method argument
def f(v); v.inspect; end
k = 0
p f(while k < 1; k += 1; end)   # "nil"

# post-test begin..end while, used as a value
m = 0
post = begin; m += 1; end while m < 3
p [post, m]         # [nil, 3]

# a plain (valueless) break still yields nil
b = 0
br = while b < 5; b += 1; break if b == 2; end
p [br, b]           # [nil, 2]

# while as the last expression of a method returns nil
def loop_ret
  c = 0
  while c < 2; c += 1; end
end
p loop_ret          # nil
