# An empty {} local written with Integer keys must settle on an int-keyed
# hash variant even when the first value is an empty container literal
# (it used to stay Str-keyed and store the int key as a pointer).
class G
  def self.bucket(ids)
    grouped = {}
    ids.each do |k|
      grouped[k] = [] if grouped[k].nil?
      grouped[k] << k
    end
    grouped.length
  end
end

puts G.bucket([1, 2, 2, 3])
h = {}
h[5] = []
h[5] << "a"
h[6] = []
p h.length
p h[5]
