# __method__ is nil at the top level; inside a method it is the method's name.
p __method__
def foo; __method__; end
p foo
def bar; __callee__; end
p bar
