threads = []
t = 0
while t < 4
  threads << Thread.new(t) do |tid|
    j = tid
    n = 0
    while j < 100_000
      s = "x" + j.to_s
      n += s.length
      j += 4
    end
    n
  end
  t += 1
end

total = 0
threads.each { |th| total += th.value }
puts total

# Regression for the multi-worker STW GC crash: a started green thread used to
# migrate between OS workers (global requeue + work stealing) while its live
# frames held compiler-cached addresses of the ORIGINAL worker's __thread data,
# corrupting both workers' GC shadow stacks. Threads are now pinned to their
# first worker. The harness runs single-worker; the parallel behaviour is
# exercised by SPINEL_WORKERS>=3 runs of this same program (issue repro).
