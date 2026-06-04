class TrueClass
  def flag_name
    "true:" + self.class.name
  end
end

class FalseClass
  def flag_name
    "false:" + self.class.name
  end
end

class Object
  def object_flag
    "object:" + self.class.name
  end
end

puts true.flag_name
puts false.flag_name

t = 1 == 1
f = 1 == 2
puts t.flag_name
puts f.flag_name

puts true.object_flag
puts false.object_flag
