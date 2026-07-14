# Block-taking methods on an empty hash (KieranP #2336)
h = {}
p(h.select { |k, v| true })
p(h.reject { |k, v| false })
h.each { |k, v| }
p h
p({}.select { |k, v| true })          # direct literal too
p({}.map { |k, v| v })
p({}.reject { |k, v| false })
p({}.count { |k, v| true })
# narrowing still wins over the empty-{} default
a = {}; a[:x] = 1
p a
b = {}; b["y"] = 2
p b
c = {}; c[3] = 4
p c
