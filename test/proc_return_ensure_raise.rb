# An exception raised inside an `ensure` that runs during a proc-return unwind
# supersedes the return: it must propagate as a real exception, not be swallowed
# while the abandoned non-local return is silently resumed.
def home
  proc {
    begin
      return 1
    ensure
      raise "from ensure"
    end
  }.call
  2
end
begin
  puts home
rescue => e
  puts "rescued: #{e.message}"
end
