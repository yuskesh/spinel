# Array conformance (KieranP #2364-#2368)
p([].unshift(1))                              # #2364 empty-literal unshift
p([].prepend(2, 3))
a = []
c = a.reduce(5, :+)                            # #2365 empty variable reduce(init, :sym)
p c
p([1, 2, 3].reduce(10, :+))
p([nil, nil].all?(nil))                        # #2366 all? with a nil pattern
p([nil, 1].all?(nil))
p([1, nil].any?(nil))
p([].rfind { |x| x > 0 })                      # #2367 rfind on an empty literal
p([1, 2, 3].fetch_values(0, 5) { |i| i * 10 }) # #2368 fetch_values block fallback
p(["a", "b"].fetch_values(1, 9) { |i| "x#{i}" })
p([1, 2, 3].fetch_values(0, 1))                # blockless still exact
begin
  [1, 2].fetch_values(9)
  p :no_error
rescue IndexError
  p :index_error
end
