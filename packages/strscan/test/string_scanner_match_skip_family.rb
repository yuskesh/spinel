# StringScanner match?/skip/exist?/skip_until/check_until, dup/clone,
# pointer-anchored ^/\A, negative [] and pos=, and scan_until match state.
require "strscan"

s = StringScanner.new("This is a test")
p s.match?(/\w+/)        # 4 (records match, does not advance)
p s.pos                  # 0
p s.matched              # "This"
p s.match?(/\d/)         # nil
p s.skip(/\w+/)          # 4 (advances)
p s.pos                  # 4
p s.exist?(/a/)          # 5 (distance to end of match, no advance)
p s.pos                  # 4
p s.skip_until(/a/)      # 5
p s.pos                  # 9
s.reset
p s.check_until(/is/)    # "This"
p s.pos                  # 0

# ^ and \A re-anchor at the scan pointer
a = StringScanner.new("This is")
a.scan(/\w+/)
p a.scan(/^\s/)          # " "
b = StringScanner.new("ab cd")
b.scan(/ab/)
p b.scan(/\A /)          # " "

# negative capture index and negative pos=
c = StringScanner.new("ab cd")
c.scan(/(\w+) (\w+)/)
p c[-1]                  # "cd"
p c[-2]                  # "ab"
p c[-9]                  # nil
d = StringScanner.new("test")
d.pos = -2
p d.rest                 # "st"
begin
  d.pos = -9
rescue RangeError => e
  puts "#{e.class}: #{e.message}"
end

# scan_until sets pre_match/post_match around the MATCH
u = StringScanner.new("This is a test")
u.scan_until(/a/)
p u.pre_match            # "This is "
p u.post_match           # " test"

# dup/clone copy scan state and share the source
o = StringScanner.new("abc")
o.scan(/a/)
dp = o.dup
p dp.string              # "abc"
p dp.pos                 # 1
dp.scan(/b/)
p dp.pos                 # 2
p o.pos                  # 1 (original unaffected)
p o.clone.rest           # "bc"
