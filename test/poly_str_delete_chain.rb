class Wad
  def initialize(blob)
    @blob = blob
  end

  def read_lump(name)
    return nil unless name == "PNAMES"

    @blob
  end
end

class Texture
  attr_reader :name

  def initialize(name)
    @name = name
  end

  def self.parse_texture(data, offset)
    name = data[offset, 8].delete("\x00").upcase
    new(name)
  end

  def self.load_all(wad)
    data = wad.read_lump("PNAMES")
    return [] unless data

    [parse_texture(data, 0), parse_texture(data, 8)]
  end
end

w = Wad.new("abcd\x00\x00\x00\x00tail\x00\x00\x00\x00rest")
Texture.load_all(w).each { |t| puts t.name }

# the plain string form keeps working
puts "hi\x00\x00\x00".delete("\x00")
puts Texture.load_all(Wad.new("x\x00\x00\x00\x00\x00\x00\x00")).map(&:name).inspect
