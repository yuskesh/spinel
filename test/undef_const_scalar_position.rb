# An unresolved constant read lowers to a runtime NameError raise whose C
# value is an sp_Class; used in a scalar slot (Array.new count, array index,
# int/float arithmetic operand) the struct used to leak into the generated C
# and fail the compile ("incompatible types ... using type 'sp_Class'").
# CRuby raises NameError at runtime -- and only when the read executes.

module Params
end

def by_count
  Array.new(Params::FRI_FINAL_MAX_LEN) { |i| i }
end

def by_index
  [1, 2][Params::FOO]
end

def by_float_arith
  1.5 * Params::RATIO
end

def by_int_arith
  2 * Params::N
end

# defined but never called: the undefined constant must stay silent
def never_called
  Array.new(Params::UNUSED) { |i| i }
end

[1, 2].each do |unused|
  # loop so the rescue sites emit once but run twice: the raise is per-read
  begin
    by_count
  rescue NameError => e
    puts e.message
  end
end

begin
  by_index
rescue NameError => e
  puts e.message
end

begin
  by_float_arith
rescue NameError => e
  puts e.message
end

begin
  by_int_arith
rescue NameError => e
  puts e.message
end

puts "done"
