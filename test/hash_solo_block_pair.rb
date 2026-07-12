# A SOLO block param on Hash Enumerables receives what CRuby yields it:
# the [k, v] PAIR for each/each_pair/map/find/sort_by/group_by/sum (their
# each yields one pair, auto-splat serves 2-param blocks), but the KEY for
# Hash#select/#reject (their own iterators yield k, v). Also the blockless
# Hash#each_with_index enumerator ([[k, v], i] pairs), and the mode-0 solo
# key binding no longer types the slot as the VALUE (a latent compile
# failure for select { |k| } on a sym-keyed hash).
h = { a: 1, b: 2 }
p h.map { |pair| pair }
p h.each_pair.map { |pair| pair }
p h.find { |pair| pair[1] == 2 }
p h.sort_by { |pair| -pair[1] }
p h.sum(0) { |pair| pair[1] }
p h.map { |k, v| [k, v] }
p h.select { |k| k == :a }
p h.reject { |k| k == :a }
p h.each_with_index.to_a
