require "json"

# scalars
p JSON.parse("42")
p JSON.parse("-7")
p JSON.parse("1.5")
p JSON.parse("true")
p JSON.parse("false")
p JSON.parse("null")
p JSON.parse('"hi\nthere"')

# arrays
p JSON.parse("[]")
p JSON.parse("[1, 2, 3]")
p JSON.parse('["a", true, null, 2.5]')

# objects: access by key, keys, size (string keys, CRuby-style)
h = JSON.parse('{"a": 1, "b": [2, 3], "c": {"d": true}}')
p h["a"]
p h["b"]
p h["c"]["d"]
p h["missing"]
p h.keys
p h.size

# empty object
p JSON.parse("{}").size

# nested array of objects
a = JSON.parse('[{"x": 1}, {"x": 2}]')
p a[0]["x"]
p a[1]["x"]

# re-generate round-trip reproduces canonical JSON
p JSON.generate(JSON.parse('{"name":"spinel","nums":[1,2,3],"ok":true}'))

# a unicode \u escape decodes to UTF-8
p JSON.parse('"é"')

# malformed input raises JSON::ParserError (caught generically)
begin
  JSON.parse("{bad}")
rescue => e
  puts e.class
end
begin
  JSON.parse("[1, 2")
rescue => e
  puts e.class
end
