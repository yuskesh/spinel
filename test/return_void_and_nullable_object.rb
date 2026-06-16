# Regression: a method returning nil in every path is typed `void`, so an
# explicit `return nil` must emit a bare `return;` rather than `return <value>;`
# from a void C function. And a method returning a nullable object with an early
# bare `return` (Ruby nil) must emit the type's nil (NULL), not a bare `return;`
# in a non-void C function. Both previously produced -Wreturn-mismatch errors.

class Pool
  def initialize
    @n = 1
  end
  def n
    @n
  end
end

class Db
  def self.open
    @pool = Pool.new
  end
  def self.close
    return if @pool.nil?
    @pool = nil
  end
  def self.pool
    @pool
  end
end

class Logger
  def self.log(msg)
    return nil if msg.empty?
    puts msg
    nil
  end
end

class Counter
  @count = 0
  def self.bump
    @count = @count + 1
    nil
  end
  def self.count
    @count
  end
  # Void method (returns nil in every path): the early `return bump` discards
  # bump's (void) value, but the side effect must still run -- codegen emits
  # `(void)(bump()); return;`, not a bare `return;` that would drop the call.
  def self.tick(run)
    return bump if run
    nil
  end
end

Db.open
p Db.pool.n
Db.close
p Db.pool.nil?
Logger.log("")
Logger.log("hi")

Counter.tick(false)
p Counter.count
Counter.tick(true)
p Counter.count
