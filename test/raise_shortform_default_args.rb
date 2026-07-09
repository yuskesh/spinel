# A short-form `raise Cls, "msg"` of an exception whose initialize has
# defaulted parameters (positional or keyword) must fill those defaults at
# the generated constructor call site: sp_Cls_new keeps the full signature,
# so a message-only emission is a cc arity error.
class Boom < StandardError
  attr_reader :level, :code
  def initialize(message, level = 7, code: 3)
    super(message)
    @level = level
    @code = code
  end
end

begin
  raise Boom, "one-arg raise"
rescue Boom => e
  puts "#{e.message} level=#{e.level} code=#{e.code}"
end

begin
  raise Boom.new("explicit", 8, code: 9)
rescue Boom => e
  puts "#{e.message} level=#{e.level} code=#{e.code}"
end
