# D-12: a module included into a class takes its ivar slot types from the
# includer, so the transplanted source scope (which codegen never emits) stops
# reading them as poly and stops widening the includer's slots back.
# Semantics must not move: a slot that is narrowed to an unboxed int/String
# still has to read `nil` when the includer never assigned it.
module Sensor
  def sample
    p @level
    p @label
    @last = @level
    p @last
  end

  def bump(n)
    n = @level
    @last = n
    n
  end
end

class Rig
  include Sensor
  def initialize(armed)
    @last = 0
    if armed
      @level = 7
      @label = "hot"
    end
  end
end

Rig.new(false).sample
Rig.new(true).sample
p Rig.new(true).bump(1)

# Two includers whose slots disagree: the module's slot is the honest join,
# so the shared body stays poly and both classes keep working.
module Tag
  def show
    @seen = @mark
    p @seen
  end
end

class IntTag
  include Tag
  def initialize; @mark = 3; @seen = 0; end
end

class StrTag
  include Tag
  def initialize; @mark = "s"; @seen = ""; end
end

IntTag.new.show
StrTag.new.show
