# case/when with a String scrutinee and string-range conditions: Range#===
# is cover? (lexicographic <=>), including exclusive, beginless, endless,
# and non-literal endpoints, in both statement and value position.
def label(c)
  case c
  when "a".."m" then "low"
  else "hi"
  end
end
p label("c")
p label("a")
p label("m")
p label("n")
p label("cat")
p label("")

def label_excl(c)
  case c
  when "a"..."m" then "low"
  else "hi"
  end
end
p label_excl("l")
p label_excl("m")
p label_excl("lz")

def label_var(c, lo, hi)
  case c
  when lo..hi then "in"
  else "out"
  end
end
p label_var("dog", "d", "f")
p label_var("cat", "d", "f")

def label_open(c)
  case c
  when ..."d" then "early"
  when "n".. then "late"
  else "mid"
  end
end
p label_open("b")
p label_open("q")
p label_open("g")

def bucket(c)
  r = case c
      when "a".."m" then 1
      when "x" then 2
      else 3
      end
  r
end
p bucket("b")
p bucket("x")
p bucket("q")
