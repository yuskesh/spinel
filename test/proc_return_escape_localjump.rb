# A non-lambda proc whose home method exits via an exception (not a normal
# return) and is called afterwards raises LocalJumpError, as in CRuby -- the
# home node is dropped as the exception unwinds the method, so the later return
# finds no live home rather than jumping into a freed stack frame.
$escaped = nil
def home
  $escaped = proc { return 99 }
  raise "boom"
end
begin
  home
rescue => e
  puts "rescued: #{e.message}"
end
begin
  $escaped.call
  puts "WRONG: no error"
rescue LocalJumpError => e
  puts "localjump: #{e.message}"
end
