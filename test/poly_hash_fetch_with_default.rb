# fetch(key, default) on a poly value that is actually a string-keyed hash
# (here the nested hash reached via the outer hash's own fetch) must perform
# the lookup, not collapse to the default. The poly-dispatch guard previously
# admitted only `[]`/include?-family calls, so a `fetch` with no user candidate
# was dropped and returned an empty string.
def pick(params)
  raw_sub = params.fetch "article", {}
  sub = if raw_sub.is_a?(Hash)
    raw_sub
  else
    {}
  end
  title = sub.fetch "title", "MISSING"
  body  = sub.fetch "body", "MISSING"
  "#{title}/#{body}"
end

puts pick({ "article" => { "title" => "hello", "body" => "world" } })
puts pick({ "article" => { "title" => "only" } })
puts pick({ "other" => { "x" => "y" } })
