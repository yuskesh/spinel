# Pass an integer-typed value where a `:ptr` arg is declared. The
# codegen arm for non-poly `:ptr` (compile_ffi_func_call in
# spinel_codegen.rb) emits `((void *)<expr>)`, so the integer's bit
# pattern flows through to C as a pointer value. Real-world use:
# pass `-1` as the `sqlite3_bind_text` destructor argument to get
# `SQLITE_TRANSIENT` (`((sqlite3_destructor_type)-1)`), the same
# wire-shape ruby-ffi exposes via `FFI::Pointer.new(-1)`. The
# round-trip was verified empirically during development against
# `sqlite3_bind_text(stmt, 1, "hello", 5, -1)` (insert + select
# returns "hello", with generated C `sqlite3_bind_text(..., ((void
# *)-1LL))` matching the SQLITE_TRANSIENT byte pattern); this test
# guards the codegen primitive that makes it possible.
#
# `free(NULL)` is a well-defined POSIX no-op, so we use it as a safe
# observation point. The interesting bit isn't the runtime behavior
# (free returns void) but that the call compiles: the analyzer must
# accept int→:ptr coercion at the call site, and codegen must emit
# `((void *)0)` — both prerequisites for sentinel-value patterns.

module LibC
  ffi_func :free, [:ptr], :void
end

# Integer literal directly as :ptr arg.
LibC.free(0)

# Int-typed local as :ptr arg.
n = 0
LibC.free(n)

puts "ok"
