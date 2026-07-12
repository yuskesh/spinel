# A one-line boolean `expr in pattern` binds the pattern's captured locals in
# the enclosing scope on a successful match (like `value => pattern`, but
# returning a boolean instead of raising).
x = [1, 2]
if x in [a, b]
  puts "matched #{a} #{b}"
else
  puts "no"
end
y = [10, 20, 30]
if y in [head, *rest]
  puts "head=#{head} rest=#{rest.inspect}"
end
h = {name: "ruby", ver: 3}
if h in {name: String => n, ver: Integer => v}
  puts "#{n} #{v}"
end
if 5 in Integer => q
  puts "q=#{q}"
end
z = [1, 2]
puts((z in [Integer, Integer]) ? "ints" : "no")
puts((z in [String, String]) ? "strs" : "no-match")
