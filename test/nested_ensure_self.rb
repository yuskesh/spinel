# A method shaped `save; begin; yield self; ensure; restore; end; self` must
# compile when it yields to a block that calls the SAME method (re-entrantly).
#
# The yielding-method inliner recorded the receiver-temp name in a file-static
# buffer that g_self points into. A nested inline (the yielded block calling the
# method again) overwrote that single static, so the outer frame's ensure and
# trailing self emitted the inner receiver temp -- "use of undeclared identifier
# '_tN'". Making the buffer stack-local gives each recursive inline its own.

class C
  attr_reader :log
  def initialize = (@log = [])
  def save = (@log << :save)
  def restore = (@log << :restore)
  def with_state
    save
    begin
      yield self
    ensure
      restore
    end
    self
  end
end

c = C.new
# 1. Single call.
c.with_state { |s| s.save }
# 2. Two-level nesting (the gap).
c.with_state { |s| s.with_state { |ss| ss.save } }
# 3. Three-level nesting.
c.with_state { |s| s.with_state { |ss| ss.with_state { |sss| sss.save } } }
p c.log.length                     #=> 15
# 4. ensure still runs when the block raises.
begin
  c.with_state { |s| raise "boom" }
rescue
end
p c.log.last                       #=> :restore

puts "done"
