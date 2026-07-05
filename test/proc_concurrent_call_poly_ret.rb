# Concurrent Proc#call return values are not corrupted across threads. A
# poly-returning proc stashes its boxed result in the _sp_proc_poly_ret slot
# (and poly arguments ride _sp_proc_poly_args); those slots were
# process-global, so concurrent calls from several threads raced on them and
# read each other's values -- intermittently wrong results, flagged
# deterministically by ThreadSanitizer. The slots are per-worker (SP_TLS)
# now, which is safe because no safepoint poll (the only migration /
# preemption point) lies between the callee's store and the call-site read.
# The loops stay allocation-free so the test exercises only the slots.

# poly return: float for even, integer for odd -- a cross-thread mixup shows
# up as a wrong value or a wrong type
pr = ->(i) { i.even? ? i * 0.5 : i * 3 }

nthreads = 4
total_n = 80000
threads = []
t = 0
while t < nthreads
  threads << Thread.new(t) do |tid|
    errs = 0
    j = tid
    while j < total_n
      v = pr.call(j)
      if j.even?
        errs += 1 unless v == j * 0.5
      else
        errs += 1 unless v == j * 3
      end
      j += nthreads
    end
    errs
  end
  t += 1
end

total = 0
threads.each { |th| total += th.value }
puts total.zero? ? "poly ret ok" : "poly ret corrupted: #{total}"
