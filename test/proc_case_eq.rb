even = ->(x) { x.even? }
puts(even === 4)
puts(even === 3)
r = case 5
when even then "even"
else "odd"
end
puts r
