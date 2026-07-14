# Integer-keyed Integer-valued hash: nesting, boxing, poly access (KieranP #2345)
p({ 1 => { 2 => 3 } })
p({ 1 => { 2 => 3 }, 4 => { 5 => 6 } })
p({ 1 => { 2 => 3 } }[1])
h = { 1 => { 2 => 3 } }
p h[1][2]
a = [{ 1 => 2 }, { 3 => 4 }]
p a
p a[0][1]
p a[1][3]
p a[0][9]
p({ 1 => 10, 2 => 20 }.is_a?(Hash))
p({ 1 => 10, 2 => 20 }.to_a)
p({ 1 => 10, 2 => 20 }.any?([1, 10]))
p({ 1 => 10, 2 => 20 }.sort_by { |k, v| -v })
p({ 1 => 10, 2 => 20 }.reduce(0) { |s, (k, v)| s + v })
