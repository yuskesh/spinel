# Issue #1306: reopening Object must not emit a second Object
# struct/constructor, and Object methods must dispatch for built-in
# receivers with the real receiver as `self`.

module ObjectGreeting
  def hi
    "hi from " + self.class.name
  end
end

class Object
  include ObjectGreeting

  def global_hi
    "global " + self.class.name
  end
end

class LocalThing
end

puts "x".hi
puts "x".global_hi
puts 1.global_hi
puts [1, 2].global_hi
puts LocalThing.new.global_hi
