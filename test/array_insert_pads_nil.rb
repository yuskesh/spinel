# Array#insert beyond the length pads with nils like CRuby, on typed
# int/str arrays (nil sentinel / NULL) and poly arrays.
b = [1, 2, 3]
b.insert(5, 8)
p b
s = %w[a b]
s.insert(4, "z")
p s
q = [1, "x"]
q.insert(4, :y)
p q
n = [1, 2]
n.insert(1, 9)
p n
