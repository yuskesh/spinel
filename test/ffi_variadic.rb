# Variadic FFI: a trailing :varargs spec makes the extern variadic (`...`) and
# passes each extra actual arg with C default promotions (int->long long,
# float->double, str->const char*). printf's signature (const char *, ...)
# matches libc, so its extern doesn't conflict. This DSL is not valid CRuby,
# so the .expected is authored against the deterministic libc behavior.
module C
  ffi_func :printf, [:str, :varargs], :int
end

# int + int + str varargs; printf returns the byte count written
n = C.printf("%d-%d-%s\n", 1, 22, "hi")
puts n

# float vararg promotes to double
C.printf("%.2f\n", 3.5)

# multiple mixed args
C.printf("[%s=%d]\n", "k", 7)

# no extra args (just the fixed format)
C.printf("plain\n")
