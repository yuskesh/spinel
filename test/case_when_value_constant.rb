# A constant holding a VALUE (symbol/int/string) used in `when` is an
# equality test, not a Module#=== class test. Treating it as a class name
# made every arm compile to `if (0)`, so the whole case fell through.

class Menu
  STATE_TITLE = :title
  STATE_OPTIONS = :options
  STATE_SKILL = 2
  LABEL = "quit"

  def initialize
    @state = STATE_TITLE
  end

  def advance(s)
    @state = s
  end

  def render
    case @state
    when STATE_TITLE then "title screen"
    when STATE_OPTIONS then "options screen"
    when STATE_SKILL then "skill select"
    when LABEL then "quitting"
    else "unknown"
    end
  end
end

m = Menu.new
puts m.render
m.advance(Menu::STATE_OPTIONS)
puts m.render
m.advance(Menu::STATE_SKILL)
puts m.render
m.advance(Menu::LABEL)
puts m.render
m.advance(:bogus)
puts m.render

# classes in `when` still work as class tests alongside value constants
def kind(x)
  case x
  when Integer then "int"
  when String then "str"
  when Menu::STATE_TITLE then "title-sym"
  else "other"
  end
end
puts kind(5)
puts kind("s")
puts kind(:title)
puts kind(3.5)

# a constant ALIASING a class must stay a Module#=== class test
class Widget; end
WidgetAlias = Widget
def alias_kind(x)
  case x
  when WidgetAlias then "widget"
  else "other"
  end
end
puts alias_kind(Widget.new)
puts alias_kind(7)
