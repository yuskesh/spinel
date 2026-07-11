# String#bytesplice and String#slice!(Regexp): byte-range replacement
# returning self, and regexp slicing returning the removed match while
# mutating the receiver and setting the match registers.

def bs(s, i, n, rep)
  t = s.dup
  r = t.bytesplice(i, n, rep)
  [r, t, r == t]
end

# happy: replace, insert (len 0), delete (empty replacement)
p bs("hello world", 0, 3, "XYZ")
p bs("hello", 1, 0, "--")
p bs("hello", 1, 3, "")

# edge: negative start counts from the end; length clamps past the end
p bs("hello", -2, 2, "LO")
p bs("hello", 1, 99, "!")
p bs("", 0, 0, "seed")

# value position: the result chains like self
def bs_chain(s)
  t = s.dup
  t.bytesplice(0, 1, "J").upcase
end
p bs_chain("hello")

# exceptional: CRuby's IndexError contracts
def bs_err(s, i, n, rep)
  t = s.dup
  t.bytesplice(i, n, rep)
rescue IndexError => e
  "#{e.class}: #{e.message}"
end
p bs_err("abc", 0, -1, "x")
p bs_err("abc", 5, 1, "x")
p bs_err("abc", -9, 1, "x")

def sl(s)
  t = s.dup
  r = t.slice!(/\d+/)
  [r, t]
end

# happy: removed match + mutated receiver; miss leaves both alone
p sl("ab12cd")
p sl("no digits")
p sl("12ab34")

# registers: capture groups from the sliced match survive
def sl_caps(s)
  t = s.dup
  t.slice!(/(\w+)@(\w+)/)
  [$1, $2]
end
p sl_caps("mail: user@example end")

# ivar receiver in statement position
class SliceBox
  def initialize(s); @s = s; end
  def chomp_digits!; @s.slice!(/\d+/); @s; end
end
p SliceBox.new("v1.2.3").chomp_digits!
