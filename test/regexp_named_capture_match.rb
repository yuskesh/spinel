# A literal regexp on the left of `=~` binds each named group to a local
# variable (CRuby's MatchWriteNode): a String when the group participated, nil
# otherwise. The `=~` itself yields the match index, or nil on no match. The
# subject string is a local so the match runs at runtime.

date = "2026-06-15"
if /(?<year>\d+)-(?<mon>\d+)-(?<day>\d+)/ =~ date
  puts year
  puts mon
  puts day
end

# the bound locals are usable as ordinary strings
if /(?<word>\w+)/ =~ "hello"
  puts word.upcase
  puts word.length
end

# interpolation with two captures
pair = "key=value"
if /(?<k>\w+)=(?<v>\w+)/ =~ pair
  puts "#{k} -> #{v}"
end

# a group that did not participate binds nil
if /(?<a>x)(?<b>y)?/ =~ "x"
  p a
  p b
end

# the value of `=~` is the match index, or nil on no match
p(/(?<n>\d+)/ =~ "abc123")
p(/(?<n>\d+)/ =~ "no digits here")

# in a ternary / value position
s = "id-7"
label = (/(?<num>\d+)/ =~ s) ? "has #{num}" : "none"
puts label

# no match leaves the captures nil
/(?<gone>\d+)/ =~ "letters"
p defined?(gone)
p gone
