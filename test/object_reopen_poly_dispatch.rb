# A method reopened on Object is callable on ANY receiver, including a boxed
# scalar inside a poly value: the poly dispatch needs it as its default arm
# (an empty cls_id switch left NULL and segfaulted).
class Tag
  def initialize(v)
    @v = v
  end
  def show
    "tag(#{@v.inspect})"
  end
end
class Object
  def tagged
    Tag.new(self)
  end
end
x = case 1
    when 2 then false
    when 1 then true
    end
puts x.tagged.show
y = case 3
    when 3 then "str"
    end
puts y.tagged.show
puts 42.tagged.show
puts nil.tagged.show
