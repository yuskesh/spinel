# Empty void base body, overridden in a subclass: the implicit-self call from
# the base method must still reach the subclass override.
class Base
  def run; validate; end
  def validate; end
end
class Sub < Base
  def validate; puts "sub-ran"; end
end
Sub.new.run
