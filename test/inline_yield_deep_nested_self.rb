# Three levels of inlined yielding methods: process (inlined via its block),
# then @a.run and @b.run2 nested inside its spliced block. The deepest block
# reads @tag through Top's self while A's and B's receivers have overwritten
# the shared inline selfbuf. The caller-self fallback must survive by value,
# not alias the clobbered buffer, or @tag resolves against the wrong receiver.
class A
  def initialize(v); @v = v; end
  def run; yield @v; end
end
class B
  def initialize(v); @v = v; end
  def run2; yield @v; end
end
class Top
  def initialize
    @tag = "TOP"
    @a = A.new(10)
    @b = B.new(20)
  end
  def process
    yield 0
    @a.run do |x|
      @b.run2 do |y|
        puts "#{@tag}: #{x} #{y}"
      end
    end
  end
end
Top.new.process { |n| puts "start #{n}" }
