# JSON.parse residuals from the multi_json mirror (issue #1853):
# symbolize_names must apply (deep, arrays included, dynamic value too), and
# the ParserError raised INSIDE the parser must match a user's
# `rescue JSON::ParserError` (the raiser uses the qualified class string so
# e.class displays like CRuby; the rescue arm now matches both forms).
require "json"

p JSON.parse('{"a":1}', symbolize_names: true)
p JSON.parse('{"a":{"b":[{"c":2}]}}', symbolize_names: true)
p JSON.parse('[{"x":1},{"y":2}]', symbolize_names: true)
p JSON.parse('{"a":1}', symbolize_names: false)
flag = [true, false].first
p JSON.parse('{"d":4}', symbolize_names: flag)

begin
  JSON.parse("{bad}")
rescue JSON::ParserError => e
  puts "caught #{e.class}"
end
begin
  raise JSON::ParserError, "direct"
rescue JSON::ParserError
  puts "control caught"
end
