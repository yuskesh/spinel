# ffi_struct declares a named C struct and generates field accessors:
# Name_new (allocate, boxed pointer), Name_get_<field> / Name_set_<field>.
# The C compiler owns the layout, so accessors use plain member access.
# (This is the spinel-native FFI DSL, not valid CRuby, so the .expected is
# authored against the deterministic behavior.)
module M
  ffi_struct :Point, [[:x, :long], [:y, :long]]
  ffi_struct :Mix,   [[:a, :int], [:d, :double]]
  ffi_struct :Node,  [[:val, :int], [:next, :ptr]]
  ffi_struct :Named, [[:label, :str], [:n, :int]]

  # a C function can fill our struct through its :ptr
  ffi_func :gettimeofday, [:ptr, :ptr], :int
  ffi_struct :Timeval, [[:tv_sec, :long], [:tv_usec, :long]]
end

# scalar fields round-trip
p = M.Point_new
M.Point_set_x(p, 3)
M.Point_set_y(p, 7)
puts M.Point_get_x(p)
puts M.Point_get_y(p)
puts(M.Point_get_x(p) + M.Point_get_y(p))

# mixed int + double
m = M.Mix_new
M.Mix_set_a(m, 42)
M.Mix_set_d(m, 3.5)
puts M.Mix_get_a(m)
puts M.Mix_get_d(m)

# pointer fields chain, NULL reads back as nil
a = M.Node_new
b = M.Node_new
M.Node_set_val(a, 11)
M.Node_set_next(a, b)
puts M.Node_get_val(a)
puts(M.Node_get_next(a) != nil)
puts(M.Node_get_next(b) == nil)

# interop: gettimeofday fills the struct through its pointer
tv = M.Timeval_new
M.gettimeofday(tv, nil)
puts(M.Timeval_get_tv_sec(tv) > 0)

# a :str field round-trips through const char* (set stores the pointer, get boxes it)
nm = M.Named_new
M.Named_set_label(nm, "hello")
M.Named_set_n(nm, 5)
puts M.Named_get_label(nm)
puts M.Named_get_n(nm)
