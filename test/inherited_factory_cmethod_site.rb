class Base
  def self.build
    rec = new
    yield rec
    rec
  end
end
class Keystore < Base
  def key=(v); @key = v; end
  def key; @key; end
  def self.upsert(k)
    build do |kv|
      kv.key = k
    end
  end
end
puts Keystore.upsert("traffic").key

class Base2
  def self.build
    rec = new
    yield rec if block_given?
    rec
  end
  def self.direct
    build { |r| r.tag }
  end
  def tag; "base"; end
end
class Mid2 < Base2
  def tag; "mid"; end
end
class Leaf2 < Mid2
  def name=(v); @name = v; end
  def name; @name; end
  def self.upsert(k)
    self.build do |kv|
      kv.name = k
    end
  end
end
p Base2.direct.tag
p Leaf2.upsert("x").name
mid = Mid2.build { |r| r.tag }
p mid.tag
