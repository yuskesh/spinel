s = "count(&:alive)"
puts s
puts s.length
puts(s == "count(&:alive)")
h = <<~EOS
  filter(&:active) and .send(:reload)
EOS
puts h
puts h.length
t = 'single &:quoted'
puts t
puts "interp works: #{[1, 2].map(&:to_s).join(",")}"
puts %w[&:a &:b].join(" ")
puts "obj.send(:name)"
# comment with &:ignored and .send(:ignored)
d = <<~'SQL'
  SELECT count(&:x) .send(:y)
SQL
puts d
puts [3, 1, 2].sort_by(&:abs).inspect
