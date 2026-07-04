# Regression: string arguments to allocating sp_str.c helpers must be
# GC-rooted. sp_str_bytes / sp_str_codepoints allocate an IntArray before
# reading the argument; when the argument is a fresh unrooted temp (an
# interpolation result) and that allocation crosses the GC threshold, the
# collection freed the argument mid-call and the result came back empty or
# garbage (observed in the Doom port: Colormap lost a row, black screen).
total = 0
lens = 0
keep = []
200.times do |i|
  n = 3000 + (i % 7) * 500
  b = "row#{i}-#{'x' * n}".bytes
  total += b.sum
  lens += b.length
  keep << b if i % 10 == 0
end
puts total
puts lens

cps = 0
clen = 0
200.times do |i|
  n = 2000 + (i % 5) * 700
  c = "cp#{i}-#{'y' * n}".codepoints
  cps += c.sum
  clen += c.length
  keep << c if i % 10 == 0
end
puts cps
puts clen
puts keep.length
