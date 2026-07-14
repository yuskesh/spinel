# A by-value (value-type) class defining #to_s: the synthesized poly to_s
# dispatch arm must dereference the boxed pointer into the by-value self,
# both when the class merely exists (compile) and when a boxed instance
# actually flows through the dispatch (runtime).

class Password
  def initialize(digest)
    @digest = digest.to_s
  end

  def to_s
    @digest
  end
end

class W
  def self.show(x)
    (x && "y").to_s
  end
end

puts W.show(1)
puts W.show(nil)

vals = [Password.new("abc"), "plain", 42]
vals.each { |v| puts v.to_s }
p (Password.new("z") && "w").to_s
x = Password.new("boxed")
p (x && x).to_s
