# An implicit-self call to a void method that subclasses override must dispatch
# on the runtime class, not bind statically to the base implementation.
class Base
  def run
    validate
  end
  def validate
    puts "base"
  end
end
class Sub < Base
  def validate
    puts "sub"
  end
end
Sub.new.run
