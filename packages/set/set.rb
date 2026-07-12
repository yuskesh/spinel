# Spinel bundled `set`.
#
# A Set backed by an Array. Elements are kept unique by ==-comparison
# (Array#include?), preserving insertion order. This covers the standard Set
# surface (construction, iteration, membership, set algebra, comparison,
# classify/divide/flatten) rather than every CRuby corner. Uniqueness of
# compound elements (arrays, structs) is only as precise as Array#include?'s
# element comparison.

class Set
  # Set[1, 2, 3] builds a Set from the given elements (CRuby's Set.[]).
  def self.[](*args)
    new(args)
  end

  def initialize(enum = nil)
    @data = []
    if enum
      if block_given?
        enum.each { |x| add(yield(x)) }
      else
        enum.each { |x| add(x) }
      end
    end
  end

  def add(x)
    @data.push(x) unless @data.include?(x)
    self
  end

  def <<(x)
    add(x)
  end

  # add? returns nil when the element was already present.
  def add?(x)
    return nil if @data.include?(x)
    @data.push(x)
    self
  end

  def delete(x)
    @data.delete(x)
    self
  end

  # delete? returns nil when the element was absent.
  def delete?(x)
    return nil unless @data.include?(x)
    @data.delete(x)
    self
  end

  def include?(x)
    @data.include?(x)
  end
  alias member? include?
  alias === include?

  def each
    @data.each { |x| yield x }
    self
  end

  def size
    @data.size
  end
  alias length size

  def empty?
    @data.empty?
  end

  def clear
    @data = []
    self
  end

  def to_a
    @data.dup
  end

  def map
    r = []
    @data.each { |x| r.push(yield(x)) }
    r
  end
  alias collect map

  # In-place block methods. map!/collect! replace the elements (re-deduplicating);
  # keep_if/delete_if always return self; select!/reject! return self when
  # anything changed and nil otherwise (CRuby's Set contract).
  def map!
    r = []
    @data.each { |x| v = yield(x); r.push(v) unless r.include?(v) }
    @data = r
    self
  end
  alias collect! map!

  def keep_if
    r = []
    @data.each { |x| r.push(x) if yield(x) }
    @data = r
    self
  end

  def delete_if
    r = []
    @data.each { |x| r.push(x) unless yield(x) }
    @data = r
    self
  end

  def select!
    n = @data.size
    r = []
    @data.each { |x| r.push(x) if yield(x) }
    @data = r
    @data.size == n ? nil : self
  end
  alias filter! select!

  def reject!
    n = @data.size
    r = []
    @data.each { |x| r.push(x) unless yield(x) }
    @data = r
    @data.size == n ? nil : self
  end

  def merge(enum)
    enum.each { |x| add(x) }
    self
  end

  def subtract(enum)
    enum.each { |x| @data.delete(x) }
    self
  end

  # Set operators build fresh sets; the operand only needs #include? /#each.
  def &(other)
    r = Set.new
    @data.each { |x| r.add(x) if other.include?(x) }
    r
  end
  alias intersection &

  def |(other)
    r = Set.new(@data)
    other.each { |x| r.add(x) }
    r
  end
  alias union |
  alias + |

  def -(other)
    r = Set.new
    @data.each { |x| r.add(x) unless other.include?(x) }
    r
  end
  alias difference -

  def ^(other)
    (self | other) - (self & other)
  end

  def ==(other)
    return false unless other.is_a?(Set)
    size == other.size && subset?(other)
  end
  alias eql? ==

  def subset?(other)
    @data.all? { |x| other.include?(x) }
  end
  alias <= subset?

  def superset?(other)
    other.subset?(self)
  end
  alias >= superset?

  def proper_subset?(other)
    size < other.size && subset?(other)
  end
  alias < proper_subset?

  def proper_superset?(other)
    size > other.size && superset?(other)
  end
  alias > proper_superset?

  def <=>(other)
    return nil unless other.is_a?(Set)
    return 0 if size == other.size && subset?(other)
    return -1 if subset?(other)
    return 1 if superset?(other)
    nil
  end

  def disjoint?(other)
    @data.each { |x| return false if other.include?(x) }
    true
  end

  def intersect?(other)
    !disjoint?(other)
  end

  # classify { |x| key } -> { key => Set of members }
  def classify
    # The nil pre-write pins k to a boxed (poly) key; see divide.
    h = {}
    @data.each do |x|
      k = nil
      k = yield(x)
      h[k] = Set.new unless h.key?(k)
      h[k].add(x)
    end
    h
  end

  # divide { |x| key } -> Set of member Sets grouped by the block value.
  # divide { |x, y| rel } -> Set of member Sets, each a strongly connected
  # component of the relation graph (mutual reachability, matching CRuby's
  # tsort-based form; a one-way relation does not merge its endpoints).
  def divide(&func)
    if func.arity == 2
      # 2-arity: strongly connected components of the relation graph (mutual
      # reachability, matching CRuby's tsort-based form; a one-way relation
      # does not merge its endpoints). reach is a flat n*n boolean closure.
      a = @data.dup
      n = a.size
      reach = []
      n.times do |i|
        n.times do |j|
          rel = i == j
          rel = true if func.call(a[i], a[j])
          reach.push(rel)
        end
      end
      n.times do |k|
        n.times do |i|
          n.times do |j|
            reach[i * n + j] = true if reach[i * n + k] && reach[k * n + j]
          end
        end
      end
      rep = []
      n.times do |i|
        ri = i
        j = 0
        while j < i
          if reach[i * n + j] && reach[j * n + i]
            ri = rep[j]
            break
          end
          j += 1
        end
        rep.push(ri)
      end
      groups = {}
      n.times do |i|
        k = rep[i]
        groups[k] = Set.new unless groups.key?(k)
        groups[k].add(a[i])
      end
      res = Set.new
      groups.each_value { |s| res.add(s) }
      res
    else
      # 1-arity: group by the block value. The nil pre-write pins k to a
      # boxed (poly) key: call sites with different block value types share
      # this body, and a concretely-typed k would collapse them to one type.
      h = {}
      @data.each do |x|
        k = nil
        k = func.call(x)
        h[k] = Set.new unless h.key?(k)
        h[k].add(x)
      end
      r = Set.new
      h.each_value { |s| r.add(s) }
      r
    end
  end

  # flatten recursively merges nested Set elements into a flat Set.
  def flatten
    r = Set.new
    @data.each do |x|
      if x.is_a?(Set)
        x.flatten.each { |y| r.add(y) }
      else
        r.add(x)
      end
    end
    r
  end

  def inspect
    "Set[" + @data.map { |x| x.inspect }.join(", ") + "]"
  end
  alias to_s inspect
end
