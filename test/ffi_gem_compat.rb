# The ffi-gem (CRuby `ffi`) compat frontend: require "ffi" is a native
# no-op, `extend FFI::Library` is declarative, attach_function (plain and
# the 4-arg rename form) lowers onto ffi_func, and the gem's type
# spellings (:string, :pointer, :uint, ...) alias the builtin specs.
require "ffi"

module LibC
  extend FFI::Library
  ffi_lib FFI::Library::LIBC
  attach_function :abs, [:int], :int
  attach_function :my_len, :strlen, [:string], :ulong
  attach_function :floor, [:double], :double
  callback :cmp, [:pointer, :pointer], :int
end

p LibC.abs(-5)
p LibC.my_len("hello")
p LibC.floor(3.7)
p LibC.abs(42)
