class D
  def initialize(t)
    @t = t
  end
  def t
    @t
  end
end
# untaken arm with a side effect must not run (File.read behind the guard)
missing = "/tmp/spinel_ternary_lazy_missing_#{Process.pid}"
d = File.exist?(missing) ? D.new(File.read(missing)) : D.new("")
puts d.t.length
# taken arm still evaluates its prelude (self-created file, platform-neutral)
present = "/tmp/spinel_ternary_lazy_present_#{Process.pid}"
File.write(present, "content")
g = File.exist?(present) ? D.new(File.read(present)) : D.new("")
puts g.t.length > 0
File.delete(present)
