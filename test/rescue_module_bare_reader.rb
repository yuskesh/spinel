# rescue-by-module matching (an exception matches a rescue arm naming a
# module its class includes) and user readers on a bare-rescued exception.

module Alertable; end
module Ignorable; end

class AppError < StandardError
  include Alertable
end
class SubAppError < AppError; end
class PlainError < StandardError; end

# direct include matches
begin
  raise AppError, "direct"
rescue Alertable => e
  puts "alertable: #{e.message}"
end

# subclass of an including class matches (module arrives via an ancestor)
begin
  raise SubAppError, "inherited"
rescue Alertable => e
  puts "alertable: #{e.message}"
end

# a class NOT including the module falls through to the next arm
begin
  raise PlainError, "plain"
rescue Ignorable
  puts "wrong arm"
rescue StandardError => e
  puts "fell through: #{e.message}"
end

# module arm chained before a class arm picks the right one
def classify(make_app)
  begin
    if make_app
      raise AppError, "app"
    else
      raise PlainError, "other"
    end
  rescue Alertable
    "matched module"
  rescue PlainError
    "matched class"
  end
end
puts classify(true)
puts classify(false)

# user reader on a bare-rescued exception
class CodedError < StandardError
  attr_reader :code
  def initialize(c); @code = c; super("coded"); end
end

begin
  raise CodedError.new(42)
rescue => err
  p err.code
  puts err.message
end

# bare rescue in a method body
def risky(n)
  raise CodedError.new(n * 2)
rescue => e
  e.code
end
p risky(21)
