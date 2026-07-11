# Complex.rect / .rectangular constructors, instance #arg (#angle/#phase),
# instance #polar and #rect component pairs.
def a1(c) = c.arg
def an(c) = c.angle
def ph(c) = c.phase
def po(c) = c.polar
def rc(c) = c.rect

p Complex.rect(3, 4)          # (3+4i)
p Complex.rectangular(3, 4)   # (3+4i)
p Complex.rect(5)             # (5+0i)
p Complex.rect(1.5, -2.5)     # (1.5-2.5i)
p a1(Complex(0, 1))           # 1.5707963267948966
p a1(Complex(1, 0))           # 0.0
p an(Complex(-1, 0))          # 3.141592653589793
p ph(Complex(0, -1))          # -1.5707963267948966
p po(Complex(3, 4))           # [5.0, 0.9272952180016122]
p rc(Complex(1.25, -2.5))     # [1.25, -2.5]
