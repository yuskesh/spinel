# Spinel FFI

Call C functions from Spinel Ruby programs. No extension compiler, no
`require "ffi"`: declarations go straight into the source and the AOT
compiler generates direct C call sites with the right externs and
linker flags.

## Example

```ruby
module LibC
  ffi_func :strlen, [:str], :size_t
  ffi_func :getpid, [],     :int
end

puts LibC.strlen("hello, world")   # 12
puts LibC.getpid
```

Compile and run:

```sh
./spinel prog.rb && ./prog
```

`libc` and `libm` are always linked; anything else needs `ffi_lib`.

## DSL reference

All FFI declarations go inside a `module` body. The module name becomes
the namespace for the functions (`RAY.InitWindow`, `LibC.strlen`, …).

### `ffi_lib "name"`

Declares that this module needs `-lname` on the link command line. May
appear multiple times per module.

```ruby
module SQL
  ffi_lib "sqlite3"
end
```

### `ffi_cflags "..."`

Declares cflags (include dirs, defines, link-search paths) needed for
this module's externs. Rarely needed — externs use standard C types
only, so headers don't have to be included in the generated code — but
useful when a library is installed somewhere non-standard.

```ruby
ffi_cflags "-I/usr/local/include"
ffi_cflags "-Wl,-rpath,/usr/local/lib"
```

### `ffi_func :name, [arg_types], ret_type`

Declares a C function callable as `Module.name(...)`.

```ruby
ffi_func :sqlite3_open,        [:str, :ptr],                   :int
ffi_func :sqlite3_close,       [:ptr],                         :int
ffi_func :sqlite3_exec,        [:ptr, :str, :ptr, :ptr, :ptr], :int
ffi_func :sqlite3_errmsg,      [:ptr],                         :str
```

Recognized type specs:

| spec | C type | Spinel type |
|---|---|---|
| `:int` | `int` | `int` |
| `:uint32` | `uint32_t` | `int` |
| `:int32` | `int32_t` | `int` |
| `:uint16` | `uint16_t` | `int` |
| `:int16` | `int16_t` | `int` |
| `:uint8` | `uint8_t` | `int` |
| `:int8` | `int8_t` | `int` |
| `:size_t` | `size_t` | `int` |
| `:long` | `long` | `int` |
| `:float` | `float` | `float` |
| `:double` | `double` | `float` |
| `:bool` | `int` | `bool` |
| `:str` | `const char *` | `string` (NUL-terminated) |
| `:binstr` | `const char *` | `string` (binary-safe, return only) |
| `:ptr` | `void *` | `ptr` |
| `:float_array` | `const double *` | `Array<Float>` (`.data` pointer) |
| `:int_array` | `const int64_t *` | `Array<Int>` (`.data` pointer) |
| `:void` | `void` | `void` (return only) |

All integer types collapse to `mrb_int` (int64) inside Spinel and are
cast to the declared C type at the call boundary. Floats collapse to
`double` the same way.

`:str` builds the result String by `strlen`, so it stops at the first
embedded NUL. `:binstr` is a return-only variant that builds a
binary-safe String of an exact byte count instead (it reads
`sp_net_bin_len`, the byte length recorded by the `sp_net` recv
functions), so embedded NUL bytes are preserved — use it for binary
socket reads where `:str` would truncate.

`:float_array` / `:int_array` hand the C side a pointer to the Spinel
Array's contiguous storage (`.data`). Length is **not** part of the
spec — pass it as a separate `:size_t` arg, same way as `:str` +
`strlen`. Lifetime is call-duration only: the GC may free the
underlying Array after the call returns, so the C side must not
stash the pointer (copy if it needs to).

### `ffi_const :NAME, <int>`

Declares an integer constant accessible as `Module::NAME`. Pure
convenience — the value is inlined at use sites like any other Ruby
integer constant.

```ruby
ffi_const :SQLITE_OK,   0
ffi_const :SQLITE_ROW,  100
ffi_const :SQLITE_DONE, 101
```

### `ffi_buffer :name, <size>`

Declares a static `size`-byte buffer, accessible as `Module.name`
returning a `:ptr`. Useful as scratch space or as an out-parameter for
functions like sqlite3's `sqlite3_open`, which writes the database
handle into a caller-supplied `sqlite3 **`.

```ruby
ffi_buffer :db_out, 8
SQL.sqlite3_open(":memory:", SQL.db_out)
db = SQL.read_ptr(SQL.db_out)  # the actual sqlite3 *
```

Lifetime: static. The buffer lives for the whole program.

### `ffi_read_u32 :name, <offset>` / `ffi_read_i32` / `ffi_read_ptr`

Declares a field reader: `Module.name(buf)` returns the value at
`offset` bytes into `buf`. Handy for poking into C structs when you
only need a few fields, or for reading back what a C function wrote
into a buffer you handed it.

```ruby
# sqlite3_open(path, ppDb) writes the new db handle into *ppDb.
# Pull the pointer out of our scratch buffer at offset 0.
ffi_read_ptr :read_ptr, 0

db = SQL.read_ptr(SQL.db_out)
```

No `ffi_write_*` in the MVP — the assumption is that a C function is
the one writing into the buffer; Ruby just reads back.

## Pointer semantics

`:ptr` maps to C `void *`. Values of this type are **not GC-tracked**:
the Spinel garbage collector never follows them and never frees them.
Foreign memory is the user's responsibility.

Two consequences worth knowing:

1. **Call destroy functions explicitly.** Nothing calls `sqlite3_close`,
   `sqlite3_finalize`, or `free()` for you.
2. **Strings passed into C are only valid for the duration of the
   call.** Spinel strings are GC-managed; if a C function stashes the
   pointer somewhere and the string becomes unreachable afterward, a
   later GC cycle will free it out from under the C code. If you need
   a string to outlive the call, copy it into an `ffi_buffer` first.

`ptr` values compare equal to `nil` when the pointer is NULL:

```ruby
db = SQL.read_ptr(SQL.db_out)
if db == nil
  puts "could not open database"
end
```

## Link-flag plumbing

The codegen emits marker comments into the generated C:

```c
/* SPINEL_LINK: -lsqlite3 */
/* SPINEL_CFLAGS: -I/usr/local/include */
```

The `spinel` compiler scrapes these from the generated C in-process and
appends them to the `cc` invocation. If you want to override (e.g. static
linking or a custom lib path), use `-c` to stop at C and drive the linker
yourself.

## Limitations

The MVP covers scalars, strings, opaque pointers, integer constants,
raw byte buffers, and simple struct-field reads. Not supported yet:

- **No struct declarations.** Use `ffi_buffer` + `ffi_read_*` for the
  handful of fields you need.
- **No callbacks / Ruby-to-C function pointers.**
- **No variadic C functions** (`printf(...)`). Use Spinel's built-in
  `printf` if you want formatted output.
- **No `ffi_write_*`** — can't write struct fields from Ruby. Pass a
  buffer to a C function that writes it for you.
- **Pointers can't enter polymorphic values.** Don't put a `:ptr` into
  a `poly_array` or a generic `Hash`; keep them as plain locals or
  wrap them in a class with a `ptr`-typed ivar.

## Examples

Runnable examples live under `examples/ffi/`:

- `examples/ffi/libm/`     — libc / libm smoke (cos, sqrt, pow, strlen, getpid)
- `examples/ffi/sqlite/`   — blog system (posts, tags, comments) on sqlite3

Each subdirectory has a `README.md` with build instructions and the
required system packages.
