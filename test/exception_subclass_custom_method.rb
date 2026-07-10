# A user-defined method (or attr_reader) on an exception subclass dispatches:
# the instance keeps its concrete class instead of collapsing to the base
# exception type, while message/class/is_a? still route through the
# exception machinery.
class MyError < StandardError
  attr_reader :code
  def initialize(code = 7)
    super("failed(#{code})")
    @code = code
  end
  def detail = "boom detail"
end

puts MyError.new.detail
e = MyError.new(42)
puts e.detail
puts e.code
puts e.message
puts e.class
puts e.is_a?(MyError)
puts e.is_a?(StandardError)
puts e.is_a?(RuntimeError)
begin
  raise MyError.new(9)
rescue MyError => ex
  puts "#{ex.class}: #{ex.message} code=#{ex.code}"
end
