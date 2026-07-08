# A block-form Enumerator.new that captures enclosing locals builds a capture
# struct handed to the generator. That struct must be GC-rooted before the
# enumerator is allocated, or a collection triggered by the enumerator's own
# allocation can reclaim it (use-after-free). Run under SPINEL_GC_STRESS=1 to
# exercise the rooting. The captured values are routed through a method so they
# are real captures, not constants.

def counter(base, step)
  Enumerator.new do |y|
    v = base
    5.times do
      y << v
      v += step
    end
  end
end

e = counter(10, 3)
p e.next
p e.next
p e.take(4)
p counter(100, -5).first(3)

# A capture that is itself heap-allocated (a string), to exercise scanning the
# rooted capture struct's pointer fields.
def labeler(prefix)
  Enumerator.new do |y|
    3.times { |i| y << "#{prefix}#{i}" }
  end
end

p labeler("x").to_a
