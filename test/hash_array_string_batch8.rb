# Hash.try_convert / ruby2_keywords_hash, Array member?/entries/find_all/
# grep(variable pattern or block)/chain, String tr_s!/delete_prefix!/
# delete_suffix!/byteindex(Regexp)/dedup.

# Hash class methods
p(Hash.try_convert({ a: 1 }))
p(Hash.try_convert("x"))
p(Hash.try_convert(nil))
p(Hash.ruby2_keywords_hash({ a: 1 }))
p(Hash.ruby2_keywords_hash?({ a: 1 }))

# Array#member? (include? alias), #entries (to_a alias)
p([1, nil, 2].member?(nil))
p([].member?(1))
p((1..3).member?(2))
a = []
p(a.entries)
b = [1, 2]
p(b.entries)

# find_all is select (blockless enumerator chain and empty receiver)
p([1, 2, 3].find_all.with_index { |x, i| i.even? })
p([].find_all { |x| x > 0 })
p([1, 2, 3, 4].find_all { |x| x.odd? })

# grep/grep_v with variable Class/Range patterns and the block form
cls = String
p([1, "a", 2, "b"].grep(cls))
r = 1..2
p([1, "a", 2, "b"].grep(r))
p([1, "a", 2, "b"].grep(Integer) { |x| x * 10 })
p([1, "a", 2, "b"].grep_v(Integer))
p([1, 2, 3].grep(2..3) { |x| x + 100 })

# eager chain
p([1, 2].chain([3, 4]).to_a)
p([1, 2].chain([3], [4, 5]).to_a)

# String bang mutators and byte searches
p "hello".tr_s!("l", "r")
p "hello".delete_prefix!("hel")
p "hello".delete_suffix!("llo")
p "abc".delete_prefix!("x")
p "hello".byteindex("l")
p "hello".byteindex(/l+/)
p "hello".byterindex("l")
p "hello".byterindex(/l+/)
p "hello".byteindex(/x/)
p "hello".dedup
p "hello".dedup.frozen?
