# Marshal writer: a repeated symbol emits a `;<index>` link (CRuby writes each
# symbol in full once), and floats use CRuby's Marshal text -- the shortest
# round-tripping %g with the exponent normalized (0.0 -> "0", 100.0 -> "1e2",
# 1e-9 -> "1e-9") -- not Ruby's to_s. Compared as raw bytes.
d = Marshal.dump([:ab, :cd, :ab])
puts d.bytes.join(",")
puts Marshal.dump(100.0).bytes.join(",")
p Marshal.load(Marshal.dump([:a, :a]))
p Marshal.load(Marshal.dump([100.0, 1e-9, 0.0]))
h = Marshal.load(Marshal.dump({a: 1, b: 2}))
p h[:a] + h[:b]
