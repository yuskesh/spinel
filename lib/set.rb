# Spinel bundled `set`.
#
# A small Set backed by an Array. Elements are kept unique by ==-comparison
# (Array#include?), preserving insertion order. This covers the common Set
# surface (construction from an enumerable, iteration, membership, add) rather
# than the full CRuby API. Uniqueness of compound elements (arrays, structs)
# is only as precise as Array#include?'s element comparison.

class Set
  def initialize(enum = nil)
    @data = []
    enum.each { |x| add(x) } if enum
  end

  def add(x)
    @data.push(x) unless @data.include?(x)
    self
  end

  def <<(x)
    @data.push(x) unless @data.include?(x)
    self
  end

  def include?(x)
    @data.include?(x)
  end

  def member?(x)
    @data.include?(x)
  end

  def each
    @data.each { |x| yield x }
  end

  def size
    @data.size
  end

  def length
    @data.size
  end

  def empty?
    @data.empty?
  end
end
