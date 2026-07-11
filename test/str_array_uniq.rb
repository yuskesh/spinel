# Array#uniq (non-bang) on a str_array — returns a fresh array with
# duplicates removed, first occurrence kept, order preserved. The
# receiver stays a str_array. Routed through an identity method to
# defeat constant folding and exercise the real typed runtime path.
def as(x); x; end

a = as(["a", "b", "a", "c", "b"])
puts a.uniq.inspect
puts a.inspect            # non-bang leaves the receiver untouched

puts as(["x", "y", "z"]).uniq.inspect
puts as([]).uniq.inspect

# built via gsub (the widened-but-still-str_array path)
u = "http://example.com/page#frag"
urls = [u.gsub(/#.*/, "")]
urls.push u.gsub(/#.*/, "")
urls.push "http://other.test"
puts urls.uniq.length
