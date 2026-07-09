# Blockless String#each_line returns an Enumerator over the lines, mirroring the
# blockless each_char enumerator.
p "a\nb\nc".each_line.to_a
p "a\nb\nc".each_line.first(2)
p "".each_line.to_a
p "nonewline".each_line.to_a
p "x\ny\nz".each_line.take(2)
e = "x\ny".each_line
p e.next
p e.next
# each_line with a block still iterates and returns self (unchanged)
buf = []
"p\nq".each_line { |l| buf << l }
p buf
