# Kernel#caller resolves to an Array (the call stack); frame *content* is
# method-granularity and only populated in --debug builds (release returns
# []), so this checks the dispatch shape that's release-stable: caller is an
# Array, and the (start, length) window is honored.
def helper
  c = caller
  puts c.class            # Array
  puts c.is_a?(Array)     # true
  puts caller(1, 0).length.to_s   # 0  (zero-length window, both runtimes)
end

helper
