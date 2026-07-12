# String#match(/re/) { |m| } yields the MatchData on a hit and evaluates
# to the block's value; nil (block not run) on a miss.
p "foobar".match(/(o+)/) { |m| m[1].upcase }
p "foobar".match(/xyz/) { |m| m[1] }
r = "count=42".match(/(\d+)/) { |m| m[1].to_i * 2 }
p r
