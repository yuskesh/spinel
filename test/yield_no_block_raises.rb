# An unguarded yield in a method called with no block raises LocalJumpError.
# A block_given?-guarded yield does not.
def run
  yield
end
begin
  run
rescue LocalJumpError => e
  puts "stmt: #{e.message}"
end

def ex
  x = yield
  x + 1
end
begin
  ex
rescue LocalJumpError => e
  puts "expr: #{e.message}"
end

def guarded
  block_given? ? yield : 42
end
puts guarded

def maybe
  yield if block_given?
  "done"
end
puts maybe

# same method used with a block still works
def twice
  yield + yield
end
puts twice { 21 }
