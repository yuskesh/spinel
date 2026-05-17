# Spike fixture: a class whose `relabel` method is never called from
# main, so without RBS seeding spinel's analyzer has no call-site
# evidence to infer `s`'s type. Default for unseen params is `int`,
# which then disagrees with @label (a string ivar) when the method
# body assigns `@label = s`. Without seeding, that disagreement
# either widens @label to poly or forces a coercion in codegen.
#
# With RBS seeding (see box.seed -> `meth relabel string string`),
# `s` starts as :string. The body assignment matches the ivar's
# declared type and no widening / coercion happens.
#
# This file alone compiles and runs identically either way -- the
# observable difference is in the analyzer's IR output for the
# Box class. See harness.rb for the diff.

class Box
  def initialize
    @label = "default"
  end

  def relabel(s)
    @label = s
    @label
  end

  def show
    @label
  end
end

b = Box.new
puts b.show
