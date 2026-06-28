# A `params = {}` filled with a computed String key is a StrStrHash. Once any
# user class defines #length, `params.length` goes through poly dispatch instead
# of the monomorphic builtin -- the dispatch switch was missing the scalar-valued
# hash arms (StrStr/StrInt/IntStr) and returned the seed 0. #inspect/#[] stayed
# correct because they never go poly. (#1614)
module App
  def self.build_params(pattern, path)
    pattern_parts = pattern.split("/")
    path_parts    = path.split("/")
    params = {}
    i = 0
    while i < pattern_parts.length
      pp = pattern_parts[i]
      ap = path_parts[i]
      params[pp[1..]] = ap if pp.start_with?(":")
      i += 1
    end
    params
  end
  # An unrelated user #length anywhere forces `.length` to poly-dispatch.
  class Flash
    def length; 99; end
  end
end

h = App.build_params("/articles/:id/comments/:cid", "/articles/7/comments/3")
puts "k0=#{h.keys[0]} k1=#{h.keys[1]}"
puts h["id"]
puts "length=#{h.length}"
puts "size=#{h.size}"
puts "empty=#{h.empty?}"

empty = App.build_params("/static", "/static")
puts "empty_len=#{empty.length}"
puts "empty_empty=#{empty.empty?}"
