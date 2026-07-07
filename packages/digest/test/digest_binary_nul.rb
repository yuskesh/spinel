# Digest inputs are spinel Strings that can carry embedded NULs (binary keys,
# salts, packed bytes). The hash must cover the whole byte length, not stop at
# the first NUL like strlen would (issue #1779).
require "digest"

z = [0].pack("C*")
puts Digest::SHA256.hexdigest("abc" + z + "def")
puts Digest::SHA256.hexdigest(z + "leading")
puts Digest::SHA1.hexdigest("abc" + z + "def")
# a NUL-free input is unchanged
puts Digest::SHA256.hexdigest("abc")
