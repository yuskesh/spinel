# Two `begin/rescue` in one scope naming DIFFERENT exception classes with the
# SAME variable used to share one slot: the binding lost its specialization and
# widened to a plain exception, so a subclass-specific reader in the first arm
# stopped compiling ("unsupported call ... recv=LocalVariableReadNode"). Only
# renaming the variable by hand got the program through.
#
# Splitting the arms apart is safe exactly when the name is confined to them.
# Ruby leaks a rescue binding past `end`, so a reader outside the arms has to
# keep sharing one slot -- the last block below is that case and must still
# compile and answer like CRuby.

class WadError < StandardError; end
class MyErr < WadError
  attr_reader :lump
  def initialize(name); @lump = name; super("bad #{name}"); end
end
class OtherErr < StandardError
  attr_reader :code
  def initialize(code); @code = code; super("code #{code}"); end
end

def f(n)
  raise MyErr.new("m") if n == 1
  raise WadError, "bad" if n == 2
  raise OtherErr.new(9) if n == 3
  42
end

# 1. the two-arm shape: the first arm reads a subclass-only attribute
begin
  f(1)
rescue MyErr => e
  puts e.lump
end
begin
  f(2)
rescue WadError => e
  puts e.message
end

# 2. three arms, two of them carrying their own state
begin
  f(1)
rescue MyErr => e
  puts "my:#{e.lump}"
end
begin
  f(3)
rescue OtherErr => e
  puts "other:#{e.code}"
end
begin
  f(0)
rescue WadError => e
  puts "wad:#{e.message}"
end
puts "no-raise ok"

# 3. arms inside a method body (a different scope) behave the same
def g
  begin
    f(1)
  rescue MyErr => x
    "lump=#{x.lump}"
  end
end
def h
  begin
    f(3)
  rescue OtherErr => x
    "code=#{x.code}"
  end
end
puts g
puts h

# 4. the binding is read AFTER `end`, so the arms must keep sharing one slot
begin
  f(1)
rescue MyErr => leaked
  puts "arm1"
end
begin
  f(2)
rescue WadError => leaked
  puts "arm2"
end
puts leaked.message
