# Class-instance-variable accessed inside a class method (def self.x).
# The emitted C class-method function has no `self`, so `@v` must lower
# to a per-class static slot rather than `self->iv_v`. Covers the
# memoized-class-method idiom, plain write+read across calls, the
# `class << self` form, name collision with an instance ivar, and a
# nil read before any write.

class Foo
  def self.cache
    @cache ||= 42
  end
end
p Foo.cache

class Cfg
  def self.label
    @label ||= "config"
  end
end
p Cfg.label
p Cfg.label

class Bar
  def self.set
    @v = 7
    @v + 1
  end
  def self.get
    @v
  end
end
p Bar.set
p Bar.get
p Bar.get

class Baz
  class << self
    def memo
      @m ||= 100
    end
  end
end
p Baz.memo

class Coll
  def initialize
    @v = "inst"
  end
  def v_inst
    @v
  end
  def self.v
    @v ||= "cls"
  end
end
o = Coll.new
p o.v_inst
p Coll.v
p o.v_inst

class NR
  def self.get
    @z
  end
end
p NR.get
