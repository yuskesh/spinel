# Regexp values render: #source/#inspect/#to_s/#options work through a
# variable (the literal-resolution arm read node attributes the variable
# lacks), p/puts print a Regexp, and boxed Regexps render inside containers.
a = /hello\d+/
p a.source
p a
p a.inspect
p a.to_s
p(/x/.source)
p [/a/, /b/]
p(/abc/.inspect)
p(/abc/.to_s)
p /abc/
puts /abc/
b = /ca.e/im
p b
p b.to_s
c1 = /pat/ix
p c1.options
p c1.source
