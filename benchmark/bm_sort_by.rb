# Array#sort_by performance across array sizes.
#
# sort_by previously lowered to a bubble sort that re-evaluated the key block on
# both operands of every comparison (O(n^2) block evaluations + O(n^2) sort). The
# Schwartzian lowering computes each key once (O(n) evaluations) and stable-sorts
# the indices with a merge sort (O(n log n)). This benchmark times sort_by at
# several sizes so the quadratic-vs-linearithmic gap is visible.
#
# The input is a deterministic multiplicative permutation (distinct keys, no
# ties), so ruby and spinel produce byte-identical sorted output regardless of
# sort stability. The sorted checksum is printed to stdout (compared by the bench
# harness); the elapsed time is printed to stderr (ignored by the harness).

def perm_array(n)
  a = []
  i = 0
  while i < n
    a << (i * 1000003) % 2000003
    i += 1
  end
  a
end

def checksum(arr)
  s = 0
  i = 0
  while i < arr.length
    s = (s + arr[i] * (i + 1)) % 1000000007
    i += 1
  end
  s
end

[8000, 16000, 32000].each do |n|
  data = perm_array(n)
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  sorted = data.sort_by { |x| x }
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  $stderr.puts "n=#{n}\t#{((t1 - t0) * 1000.0).round(4)} ms"
  puts "n=#{n} checksum=#{checksum(sorted)}"
end
puts "done"
