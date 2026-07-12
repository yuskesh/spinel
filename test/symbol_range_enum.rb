# (:a..:e): a symbol-endpoint range enumerates by name succession --
# the range lowers to a poly array of boxed symbols (to_a/map/each/
# include?, exclusive ends, multi-char names).
p (:a..:e).to_a
p((:a..:c).map(&:to_s))
p (:a...:d).to_a
acc = []
(:x..:z).each { |s| acc << s }
p acc
p (:a..:e).include?(:c)
p (:aa..:ac).to_a
