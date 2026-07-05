class Inner
  def initialize(v)
    @val = v
  end
  # a yielding method; inlined at the call site
  def run
    yield @val
  end
end

class Outer
  def initialize
    @tag = "outer"
    @inner = Inner.new(7)
  end

  # yielding method that, in its body, calls Inner#run with a block.
  # The block reads @tag through Outer's self -- while Inner#run is inlined
  # (its self is the Inner receiver). Nested inline must not clobber the
  # caller's self used by the spliced block body.
  def process
    yield 1
    @inner.run do |x|
      puts "#{@tag}: #{x}"
    end
  end
end

o = Outer.new
o.process { |n| puts "start #{n}" }
