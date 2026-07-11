# Regexp capture plumbing: named-group scan, block-form scan destructuring,
# the \+ replacement backref, non-string gsub block values, and blockless
# each_line(chomp:).

def scn(s)
  s.scan(/(?<a>\w)(?<b>\w)/)
end
p scn("abcd")
p scn("x")

# lookbehinds are not capture groups: whole matches come back
def slb(s)
  s.scan(/(?<=a)\d/)
end
p slb("a1 b2 a3")

# numbered groups through the block form: params destructure the row
def scb(s)
  out = []
  s.scan(/(\d)(\d)/) { |a, b| out << [a, b] }
  out
end
p scb("12 34")

# an optional group that does not participate binds nil
def sco(s)
  out = []
  s.scan(/(a)(x)?/) { |a, b| out << [a, b] }
  out
end
p sco("ab")

# a single param binds the whole group row
def scr(s)
  rows = []
  s.scan(/(\d)(\w)/) { |row| rows << row }
  rows
end
p scr("1a 2b")

# \+ expands to the highest-numbered participating group
def bp(s, r)
  s.sub(/(a)(b)?/, r)
end
p bp("ab", '\+')
p bp("a.", '\+')
p bp("xy", '\+')

def bg(s)
  s.gsub(/(\d)([a-z])?/, '<\+>')
end
p bg("1a 2. 3c")

# gsub block values stringify like CRuby (Integer, nil, Float)
def gi(s)
  s.gsub(/(\d)/) { $1.to_i * 2 }
end
p gi("a1b2")

def gn(s)
  s.gsub(/x/) { nil }
end
p gn("axbxc")

def gf(s)
  s.gsub(/\d+/) { |m| m.to_f / 2 }
end
p gf("10 33")

# blockless each_line with the chomp keyword materializes an Enumerator
def elc(s)
  s.each_line(chomp: true).to_a
end
p elc("a\nb\nc")
p elc("one\n\ntwo\n")

def elk(s)
  s.each_line(chomp: false).to_a
end
p elk("a\nb")

def elm(s)
  s.each_line(chomp: true).map { |ln| ln.upcase }
end
p elm("x\ny\n")
