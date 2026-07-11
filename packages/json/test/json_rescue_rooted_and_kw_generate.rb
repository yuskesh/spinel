# Two multi_json-mirror edges: a root-anchored `rescue ::JSON::ParserError`
# must match the parser's raise like the unanchored form, and JSON.generate
# through a POLY-widened parameter must serialize a packed-keywords hash
# (one param fed both a string-keyed and a symbol-keyed hash).
require "json"

r1 = begin; JSON.parse("{bad}"); rescue JSON::ParserError; "caught"; rescue => e; "leak:#{e.class}"; end
p r1
r2 = begin; JSON.parse("{bad}"); rescue ::JSON::ParserError; "caught"; rescue => e; "leak:#{e.class}"; end
p r2

def g(o); JSON.generate(o); end
p g({ "a" => 1 })
p g(:k => "v")
p g({ x: [1, { "y" => true }] })
