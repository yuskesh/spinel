# A yielding `initialize` is inlined at the `new { }` call site. Its body
# runs in the CONSTRUCTED class's context, so an implicit-self call there
# (`setup`) must resolve against Builder, not the caller Client — while
# constructor arguments stay call-site code (bound in the caller's
# context, handled separately).
class Builder
  def initialize(seed)
    @seed = seed
    setup                 # implicit-self call inside the yielding initialize
    yield self
  end

  def setup
    @ready = true
  end

  def ready?
    @ready
  end

  def seed
    @seed
  end
end

class Client
  def base
    100
  end

  def make
    # `base` (caller's method) is an ARG => caller context;
    # `setup` inside initialize => constructed-class context.
    Builder.new(base) { |b| b }
  end
end

w = Client.new.make
p w.ready?
p w.seed
