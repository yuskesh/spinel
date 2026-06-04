# Built-in class reopens should not be collected as user classes.
# Array uses the boxed open-class receiver path rather than emitting
# a fake sp_Array struct.

class Array
  def marker
    self.class.name
  end
end

puts [1, 2].marker
