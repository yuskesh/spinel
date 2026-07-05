# A poly component of File.join (here: an element read from a heterogeneous
# array) initializes a `const char *` slot in the sp_file_join argument list,
# so it must be coerced with sp_poly_to_s rather than land its sp_RbVal raw
# (doom's SoundManager builds its temp dir from a poly Dir.tmpdir value).
parts = ["sounds", 7]
dir = parts[0]
path = File.join(dir, "doom_rb")
puts path
puts File.join("a", "b", "c")
