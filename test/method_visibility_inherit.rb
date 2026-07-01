# Visibility resolves up the ancestor chain: an inherited method keeps its
# defining class's visibility, and a subclass may re-declare it (private :m /
# public :m). respond_to? / method_defined? reflect the effective visibility.

class Base
  def base_pub; 1; end

  private

  def base_priv; 2; end
end

class Sub < Base
  private :base_pub      # demote an inherited public method
  public :base_priv      # promote an inherited private method
  def own_pub; 3; end
end

class AttrBase
  private

  def val; 9; end
end

class AttrSub < AttrBase
  attr_reader :val       # public attr overrides an inherited private method
end

# inherited, not re-declared: keep Base's visibility
puts Base.new.respond_to?(:base_pub)
puts Base.new.respond_to?(:base_priv)
puts Base.method_defined?(:base_pub)
puts Base.method_defined?(:base_priv)

# re-declared in Sub: effective visibility flips
puts Sub.new.respond_to?(:base_pub)        # demoted -> false
puts Sub.new.respond_to?(:base_pub, true)  # include_all -> true
puts Sub.new.respond_to?(:base_priv)       # promoted -> true
puts Sub.new.respond_to?(:own_pub)
puts Sub.method_defined?(:base_pub)        # demoted -> false
puts Sub.method_defined?(:base_priv)       # promoted -> true
puts Sub.private_method_defined?(:base_pub)
puts Sub.public_method_defined?(:base_priv)

# a public attr in a subclass overrides an inherited private method
puts AttrBase.new.respond_to?(:val)        # inherited private -> false
puts AttrSub.new.respond_to?(:val)         # public attr override -> true
puts AttrSub.public_method_defined?(:val)  # -> true
