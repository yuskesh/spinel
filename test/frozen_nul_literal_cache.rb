# frozen_string_literal: true
# `fresh_receiver.delete("\0")` must not corrupt the receiver. Before the fix
# the frozen "\0" literal was re-allocated on every call, and that allocation
# could trigger a GC that swept the (unrooted) fresh receiver before delete
# read it -- a use-after-free (doom's `data[8,8].delete("\0")` WAD name parse).
# Here the receiver has no NUL, so delete is a no-op returning the receiver.
out = []
8000.times do |i|
  out << "item#{i % 100}".delete("\x00")
end
puts out.length
puts out[0]
puts out.last
bad = 0
out.each_with_index { |s, i| bad += 1 unless s == "item#{i % 100}" }
puts "bad=#{bad}"
