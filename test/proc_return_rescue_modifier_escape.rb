# Same home-escape-via-exception case as proc_return_escape_localjump, but the
# exception is caught by an *expression-form* `rescue` modifier (a distinct
# codegen path). The home node must still be dropped as the exception unwinds,
# so the later return raises LocalJumpError rather than longjmping into a freed
# frame.
$escaped = nil
def home
  $escaped = proc { return 99 }
  raise "boom"
end
v = (home rescue 123)
p v
begin
  $escaped.call
  puts "WRONG: no error"
rescue LocalJumpError => e
  puts "localjump: #{e.message}"
end
