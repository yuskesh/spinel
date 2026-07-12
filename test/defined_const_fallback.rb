# A defined?-guarded constant fallback folds to its live arm in value
# position: the dead arm may reference the missing constant itself, and it
# never evaluates (no NameError), matching CRuby.
class Foo
  VER = "2.5"
end

v = defined?(Foo::BAR) ? Foo::BAR : "1.0.0"
p v

def pick = defined?(Foo::BAR) ? Foo::BAR : "fallback"
p pick

class Holder
  def initialize
    @v = defined?(Foo::MISSING) ? Foo::MISSING : "default"
  end
  attr_reader :v
end
p Holder.new.v

# defined-true side: the dead fallback references a missing constant
w = defined?(Foo::VER) ? Foo::VER : Foo::NOPE
p w
x = defined?(String) ? "have-string" : Absent::Thing
p x

# unless polarity
y = "set" unless defined?(Foo::BAR)
p y

# statement form (both polarities)
if defined?(Foo::BAR)
  p Foo::BAR
else
  p "no-bar"
end
if defined?(Foo::VER)
  p "have-ver"
else
  p "no-ver"
end

# elsif chain continues as the value
z = if defined?(Foo::BAR)
      Foo::BAR
    elsif defined?(Foo::VER)
      Foo::VER
    else
      "neither"
    end
p z
