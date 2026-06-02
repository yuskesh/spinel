# A qualified constant supplied by a require Spinel cannot honor used to
# emit a bare `Mylib_VERSION` C identifier (undeclared -> C compile error).
# It now degrades to a runtime NameError, mirroring an unqualified
# unresolved constant, so the program compiles and the error is rescuable.
require "mylib/version"
begin
  puts Mylib::VERSION
rescue NameError => e
  puts "rescued: #{e.message}"
end
puts "after"
