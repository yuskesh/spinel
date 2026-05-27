# Time.mktime / Time.local / Time.utc — all three name the local
# (and gm/utc) constructor. Time.now and Time.new were already
# wired; this test covers the aliases plus the UTC variant that
# uses a timezone-independent days-from-civil epoch.
t = Time.now
puts t.class

t2 = Time.mktime(2025, 1, 15, 10, 30, 0)
puts t2.year
puts t2.month
puts t2.day
puts t2.hour
puts t2.min

t3 = Time.local(2026, 3, 5)
puts t3.year
puts t3.month
puts t3.day

t4 = Time.utc(2025, 6, 15, 12, 0, 0)
puts t4.year
puts t4.month
puts t4.day
puts t4.hour
