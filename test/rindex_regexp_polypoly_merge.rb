# String#rindex(Regexp) keys on the rightmost match START (a later-starting
# shorter match beats an earlier longer one), and a heterogeneous hash
# absorbs a typed hash through merge!/update.

p "hello".rindex(/l+/)
p "aaa".rindex(/a+/)
p "héllo".rindex(/l+/)
p "abc".rindex(/x/)
p "banana".rindex(/an/)

h = { 1 => "a", :b => 2 }
h.merge!({ "c" => 3.5 })
p h
h.update({ d: 4 }, { 5 => "e" })
p h
