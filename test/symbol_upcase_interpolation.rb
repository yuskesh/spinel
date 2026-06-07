# Symbol#upcase / #downcase on a method-returned (branch-poly) symbol
# inside string interpolation lowers through sp_sym_intern. The runtime
# helper is gated on @needs_sym_intern, which analyze set as a memoized
# side effect and missed for this shape -> link failure on the call.
# Found via the spinelgems harness (nagiosplugin).
class P
  def status(n)
    return :critical if n > 2
    return :warning if n > 1
    :ok
  end
  def output(n)
    "X #{status(n).upcase}"
  end
  def lower(n)
    "y #{status(n).downcase}"
  end
end

puts P.new.output(3)
puts P.new.output(2)
puts P.new.output(1)
puts P.new.lower(3)
puts "hi".to_sym.inspect
