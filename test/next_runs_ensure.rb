r = []
[1].each do |x|
  begin
    r << :begin
    next
    r << :nope
  ensure
    r << :ensure
  end
end
p r
r2 = []
[1, 2].each do |x|
  begin
    r2 << :outer_begin
    begin
      r2 << :inner_begin
      next if x == 1
    ensure
      r2 << :inner_ensure
    end
    r2 << :after
  ensure
    r2 << :outer_ensure
  end
end
p r2
r3 = []
begin
  [1].each do |x|
    r3 << :in
    next
  end
  r3 << :out
ensure
  r3 << :ens
end
p r3
