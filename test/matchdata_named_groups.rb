# MatchData named-group access: md[:name] / md["name"] select a named capture
# group (previously returned the whole match), plus #named_captures and #names.
# A non-participating named group is nil; an unknown name raises IndexError.
def s(x); x; end

m = "2026-06".match(/(?<y>\d+)-(?<mo>\d+)/)
p m[:mo]
p m[:y]
p m["mo"]
p m.named_captures
p m.names
# positional indexing is unchanged
p m[0]
p m[1]

# non-literal symbol key routes through the runtime path
k = s(:mo)
p m[k]

# non-participating named group -> nil (value side is poly)
alt = "x".match(/(?<a>x)|(?<b>y)/)
p alt[:a]
p alt[:b]
p alt.named_captures

# no named groups
plain = "ab".match(/(a)(b)/)
p plain.named_captures
p plain.names

# unknown name raises IndexError
begin
  p m[:nope]
rescue => e
  puts "#{e.class}: #{e.message}"
end
