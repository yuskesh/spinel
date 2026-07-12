# A valued break out of Kernel#loop is the value of the loop -- and, when the
# loop is a method's tail expression, of the method.
def factor; i = 0; loop { i += 1; break i * 10 if i >= 3 }; end
p factor
def labelled; n = 0; r = loop { n += 1; break "done:#{n}" if n == 2 }; r; end
p labelled
def novalue; x = 0; loop { x += 1; break if x > 5 }; x; end
p novalue
def after; loop { break }; 99; end
p after
