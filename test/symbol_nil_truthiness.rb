# A nilable symbol slot stores (sp_sym)-1 as its nil sentinel. Truthiness
# in conditions and in the value form of && / || must test the sentinel;
# it used to read always-true, so `if @exit_triggered` fired on nil.

class Level
  def initialize
    @exit_triggered = nil
  end

  def trigger(kind)
    @exit_triggered = kind
  end

  def check
    if @exit_triggered
      "exit:#{@exit_triggered}"
    else
      "playing"
    end
  end

  def check_and(intermission)
    # value-form && with a symbol left arm
    (@exit_triggered && !intermission) ? "finish" : "continue"
  end

  def sym_or_default
    @exit_triggered || :none
  end
end

lvl = Level.new
puts lvl.check
puts lvl.check_and(false)
p lvl.sym_or_default
lvl.trigger(:normal)
puts lvl.check
puts lvl.check_and(false)
puts lvl.check_and(true)
p lvl.sym_or_default

# ||= / &&= on nilable symbol slots (ivar and local, statement and value form)
class Cache
  def initialize
    @mode = nil
  end
  def mode_or_default
    @mode ||= :easy
  end
  def try_upgrade
    @mode &&= :hard
    @mode
  end
  def reset
    @mode = nil
  end
end

c = Cache.new
p c.mode_or_default
p c.mode_or_default
p c.try_upgrade
c.reset
p c.try_upgrade

def lsym(start)
  s = start ? :given : nil
  s ||= :fallback
  got = (s &&= :promoted)
  [s, got]
end
p lsym(true)
p lsym(false)
