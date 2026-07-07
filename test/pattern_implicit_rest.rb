# A trailing comma in an array pattern (`in [0, 1, ]`) is an implicit rest:
# at-least length semantics, binding nothing.
case [0, 1, 2, 3]
in [0, 1, ] then puts "matched"
else puts "no"
end
case [5]
in [0, 1, ] then puts "wrong"
else puts "no2"
end
