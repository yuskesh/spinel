# Visibility applies to attr_accessor / attr_reader / attr_writer methods
# declared under a bare private/protected section (writers tracked as "name=").

class Box
  attr_reader :open_r
  attr_accessor :open_rw

  private

  attr_accessor :hidden       # hidden + hidden= are private
  attr_reader :hidden_r

  protected

  attr_writer :prot_w         # prot_w= is protected

  public
end

def sorted(arr); arr.map(&:to_s).sort; end

puts sorted(Box.instance_methods(false)).inspect
puts sorted(Box.private_instance_methods(false)).inspect
puts sorted(Box.protected_instance_methods(false)).inspect

puts Box.method_defined?(:open_r)
puts Box.method_defined?(:open_rw=)
puts Box.method_defined?(:hidden)
puts Box.method_defined?(:hidden=)
puts Box.private_method_defined?(:hidden)
puts Box.private_method_defined?(:hidden_r)
puts Box.protected_method_defined?(:prot_w=)

b = Box.new
puts b.respond_to?(:open_r)
puts b.respond_to?(:open_rw=)
puts b.respond_to?(:hidden)
puts b.respond_to?(:hidden, true)
puts b.respond_to?(:hidden_r, true)
