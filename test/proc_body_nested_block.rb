class Defer
  def initialize
    @blocks = []
  end

  def add(&blk)
    @blocks.push(blk)
  end

  def call_all
    @blocks.each do |b|
      b.call
    end
  end
end

class Runner
  def self.with_deferred(&outer)
    d = Defer.new
    outer.call(d)
    d.call_all
  end
end

Runner.with_deferred do |d|
  d.add { puts "first" }
  d.add { puts "second" }
  puts "mid"
end
