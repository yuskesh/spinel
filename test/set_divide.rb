require 'set'
s = Set[1, 2, 3, 4, 5, 6]
p s.divide { |x| x % 3 }.map { |sub| sub.to_a.sort }.sort
p Set[1, 2, 3, 4].divide { |x, y| (x - y).abs == 1 }.map { |sub| sub.to_a.sort }.sort
p Set[1, 2, 3].divide { |x, y| x - y == 1 }.map { |sub| sub.to_a.sort }.sort
