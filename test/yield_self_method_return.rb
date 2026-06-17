module M
  def self.go
    yield
  end
end
puts M.go { 7 }

class K
  def self.twice
    yield + yield
  end
end
puts K.twice { 5 }

module W
  def self.wrap
    "[" + yield + "]"
  end
end
puts W.wrap { "hi" }
puts W.wrap { "hi" }.to_s
v = M.go { 7 }
puts Integer(v)
