# A SOLO block param on Hash Enumerables receives what CRuby yields it:
# the [k, v] PAIR for each/each_pair/map/find/sort_by/group_by/sum (their
# each yields one pair, auto-splat serves 2-param blocks), but the KEY for
# Hash#select/#reject (their own iterators yield k, v). Also the blockless
# Hash#each_with_index enumerator ([[k, v], i] pairs), and the mode-0 solo
# key binding no longer types the slot as the VALUE (a latent compile
# failure for select { |k| } on a sym-keyed hash).
