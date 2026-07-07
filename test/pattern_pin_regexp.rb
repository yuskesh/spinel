# A pinned Regexp in a pattern matches via === (Regexp#===), not equality.
r = /a/
case 'abc'
in ^r then puts "re ok"
else puts "no"
end
x = 5
case 5
in ^x then puts "eq ok"
end
case 'zzz'
in ^r then puts "wrong"
else puts "no2"
end
