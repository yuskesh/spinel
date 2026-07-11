# Blockless String#each_char / each_line / each_byte / each_codepoint
# return Enumerators stamped with the ORIGINAL string receiver and creating
# method, so #inspect shows #<Enumerator: "abc":each_char> -- not the
# materialized snapshot array with a generic :each. Iteration still drains
# the snapshot (next/to_a unchanged).
p("abc".each_char)
p("a\nb\n".each_line)
p("ab".each_byte)
p("ab".each_codepoint)
p("x\ny\n".each_line(chomp: true))
e = "hi".each_char
p e.next
p e.next
p e.to_a
