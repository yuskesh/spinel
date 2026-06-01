# A String range has no integer size; CRuby Range#size returns nil.
# Integer ranges keep returning their element count.
p (1..10).size
p (1...10).size
p ("a".."z").size
p ("a".."z").size.nil?
p ("a".."e").size
