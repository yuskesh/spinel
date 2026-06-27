# A polymorphic (untyped) receiver's method dispatch enumerates every user
# class defining that method name. A class whose definition *yields* is inlined
# at its call sites and never emitted as a standalone function, so the dispatch
# must drop its `case` arm instead of calling an absent symbol (which would
# dangle at link). Here `Cache#fetch` yields and is never the actual receiver;
# `Flash#fetch` is a plain method. The poly receiver below (read out of an
# array, so its static type is the top type) reaches a `fetch` dispatch that
# enumerates both -- only the linkable arm may be emitted.
class Cache
  def fetch(key, opts = {})
    yield
  end
end

class Flash
  def fetch(key, default = nil)
    default
  end
end

arr = []
h = {}
h["x"] = "1"
arr << h
sub = arr[0]
puts sub.fetch("x", "0").to_s
puts sub.fetch("absent", "default").to_s
