# x.is_a?(NoSuchConst): the is_a?-family arms match the target constant
# textually and never emit the constant read, so an undefined constant
# silently answered false where CRuby raises NameError.
[5, "s", String].each do |x|
  begin
    x.is_a?(NoSuchConst)
    puts "no raise"
  rescue NameError => e
    puts "NameError: #{e.message}"
  end
end
# defined targets keep their answers
p 5.is_a?(Integer)
p 5.is_a?(Comparable)
p String.is_a?(Module)
p String.is_a?(Comparable)
