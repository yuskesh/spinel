# A valued break from a while used in value position yields that value.
def s(x); x; end
n = s(0)
r = while true
  n += 1
  break "stopped at #{n}" if n == 3
end
p r
m = s(0)
r2 = while true
  m += 1
  break m * 10 if m == 5
end
p r2
