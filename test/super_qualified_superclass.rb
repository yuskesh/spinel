module A
  module B
    class Handler
      def initialize; @req = 1; end
      def greet; "hi from #{@req}"; end
    end
  end
end
class Child < A::B::Handler
  def initialize
    super
    @ws = 2
  end
end
c = Child.new
puts c.greet
puts "ok"
