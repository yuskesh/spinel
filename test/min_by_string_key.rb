# min_by/max_by with a String or Symbol block key orders lexicographically
# (String#<=>), not just for Integer/Float keys.
p ["banana", "apple", "cherry"].min_by { |s| s }
p ["banana", "apple", "cherry"].max_by { |s| s }
p ["banana", "apple", "cherry"].max_by { |s| s.length }
p [:foo, :bar, :baz].min_by { |s| s }
def first_alpha(a); a.min_by { |s| s }; end
p first_alpha(["c", "a", "b"])
