# Forwarding a `**hash` into a callee's keyword-rest (`**opts`) parameter.
# The forwarded hash's entries must reach `opts`; the kwrest param keeps its
# Symbol-keyed hash type (it receives the whole hash, not a value type).
def collect(**opts)
  opts
end

h = {a: 1, b: 2}
p collect(**h)            # {a: 1, b: 2}
p collect(c: 3, **h)      # {c: 3, a: 1, b: 2}  -- literal before splat
p collect(**h, a: 9)      # {a: 9, b: 2}        -- splat then literal (last wins)

# Reading a single key back out of the kwrest hash.
def fetch_a(**opts)
  opts[:a]
end
p fetch_a(**h)            # 1

# A named keyword param alongside a kwrest: the named key is consumed,
# the rest flow into the kwrest hash.
def split(a:, **rest)
  [a, rest]
end
abc = {a: 1, b: 2, c: 3}
p split(**abc)            # [1, {b: 2, c: 3}]

# A positional param of the same name does NOT consume a forwarded key: the
# key stays in the keyword-rest (only keyword params consume).
def split_positional(a, **rest)
  [a, rest]
end
p split_positional(1, **abc)   # [1, {a: 1, b: 2, c: 3}]

# Forwarding the kwrest onward to another **kw callee.
def outer(**opts)
  collect(**opts)
end
p outer(x: 1, y: 2)       # {x: 1, y: 2}
