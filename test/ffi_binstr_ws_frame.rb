# Regression test for matz/spinel#1466 / ac1e0d2c: the FFI :binstr return mode
# must preserve a binary payload's embedded NUL bytes, where the strlen-based
# :str mode truncates. The motivating case is a WebSocket frame: an inbound
# (client->server) frame is masked, and an ActionCable "subscribe" payload is
# >125 bytes, so the WS extended 2-byte length field is used -- its high byte is
# 0x00 (lengths 126..255) at frame index 2. Under :str the masked frame is
# truncated there (the server never reaches the masking key or payload and can't
# unmask); under :binstr every byte survives and the payload unmasks exactly.
#
# Deterministic + POSIX-portable: the frame bytes are delivered through
# shell_capture (a printf of octal escapes), the same channel ffi_binstr_recv.rb
# / sp_net_basic.rb use, so the .expected holds on Linux + macOS CI. This guards
# the :binstr FFI lowering itself; the live sp_net_recv_some socket path is
# exercised by the consumer suites (tep) and the repro on the issue.
module Bin
  ffi_func :sp_net_shell_capture, [:str, :int], :binstr
end
module Str
  ffi_func :sp_net_shell_capture, [:str, :int], :str
end

# Expected unmasked payload (the ActionCable subscribe JSON), as bytes.
PAYLOAD = [123, 34, 99, 111, 109, 109, 97, 110, 100, 34, 58, 34, 115, 117, 98, 115, 99, 114, 105, 98, 101, 34, 44, 34, 105, 100, 101, 110, 116, 105, 102, 105, 101, 114, 34, 58, 34, 123, 92, 34, 99, 104, 97, 110, 110, 101, 108, 92, 34, 58, 92, 34, 82, 111, 111, 109, 67, 104, 97, 110, 110, 101, 108, 92, 34, 44, 92, 34, 114, 111, 111, 109, 92, 34, 58, 92, 34, 103, 101, 110, 101, 114, 97, 108, 45, 100, 105, 115, 99, 117, 115, 115, 105, 111, 110, 45, 108, 111, 98, 98, 121, 92, 34, 44, 92, 34, 115, 105, 110, 99, 101, 92, 34, 58, 49, 55, 49, 56, 54, 48, 48, 48, 48, 48, 125, 34, 125].pack("C*")

# The masked WebSocket frame for that payload, as a printf of octal escapes.
CMD = "printf '\\201\\376\\000\\177\\067\\372\\041\\235\\114\\330\\102\\362\\132\\227\\100\\363\\123\\330\\033\\277\\104\\217\\103\\356\\124\\210\\110\\377\\122\\330\\015\\277\\136\\236\\104\\363\\103\\223\\107\\364\\122\\210\\003\\247\\025\\201\\175\\277\\124\\222\\100\\363\\131\\237\\115\\301\\025\\300\\175\\277\\145\\225\\116\\360\\164\\222\\100\\363\\131\\237\\115\\301\\025\\326\\175\\277\\105\\225\\116\\360\\153\\330\\033\\301\\025\\235\\104\\363\\122\\210\\100\\361\\032\\236\\110\\356\\124\\217\\122\\356\\136\\225\\117\\260\\133\\225\\103\\377\\116\\246\\003\\261\\153\\330\\122\\364\\131\\231\\104\\301\\025\\300\\020\\252\\006\\302\\027\\255\\007\\312\\021\\255\\112\\330\\134'"

full  = Bin.sp_net_shell_capture(CMD, 4096)
trunc = Str.sp_net_shell_capture(CMD, 4096)
puts "frame bytes (binstr)  = #{full.bytesize}"
puts "truncated (str)       = #{trunc.bytesize}"

# Unmask the full frame: 2-byte extended length at [2..3], 4-byte key at [4..7],
# masked payload from [8].
plen = (full.getbyte(2) << 8) | full.getbyte(3)
kk = [full.getbyte(4), full.getbyte(5), full.getbyte(6), full.getbyte(7)]
out = []
i = 0
while i < plen
  out << (full.getbyte(8 + i) ^ kk[i & 3])
  i += 1
end
puts "unmasked length       = #{out.length}"
puts "unmasked == subscribe = #{out.pack("C*") == PAYLOAD}"
