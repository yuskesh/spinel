# JSON.pretty_generate: CRuby's two-space-indented format, ": " separator,
# closer at the parent indent, empty containers inline. Previously an
# unresolved call (the multi_json mirror gap).
require "json"
puts JSON.pretty_generate({ "a" => 1, "b" => [1, 2] })
puts JSON.pretty_generate([1, { "k" => "v" }, [true, nil]])
puts JSON.pretty_generate({})
puts JSON.pretty_generate([])
puts JSON.pretty_generate({ "s" => "x\"y", "f" => 2.5, "n" => nil, "deep" => { "e" => {} } })
p JSON.parse(JSON.pretty_generate({ "round" => ["trip", 1] }))
