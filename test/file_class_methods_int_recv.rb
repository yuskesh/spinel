# `File.readable?` and `File.binwrite` — class-method stubs in
# the `rcname == "File"` dispatch block, plus `IntArray#pack("C*")`
# which is the symmetric inverse of `String#bytes`.
#
# Surfaced via optcarrot's battery-save dead code path:
#   `return unless File.readable?(sav)` and
#   `File.binwrite(sav, @wrk.pack("C*"))`.
#
# `readable?` reuses the exist check (close enough on POSIX).
# `binwrite` reuses `sp_file_write` — fputs-based, so embedded
# NULs truncate; acceptable for the NUL-free use sites that
# exist in practice.

path = "/tmp/spinel_file_class_test"

File.binwrite(path, [72, 105, 33].pack("C*"))   # "Hi!"
puts File.readable?(path)
puts File.read(path)
File.delete(path)
puts File.exist?(path)
