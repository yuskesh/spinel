# Element sub-pattern checks and binding descent in container patterns:
# class/alternation/range guards on array elements, nested hash patterns
# under hashes and arrays, find patterns nested in hash values, and
# capture bindings inside find windows.

# class/constant element guards on a mixed (poly) array
def cls_guard(x); case x; in [String, Integer]; :match; else :no; end; end
p cls_guard([1, "s"])
p cls_guard(["s", 1])

# alternation as an array element (typed int array scrutinee)
def alt_elem(x); case x; in [0 | 1 | 2]; "low"; else "other"; end; end
p alt_elem([10])
p alt_elem([1])

# alternation as a hash value
def alt_hval(h); case h; in {a: 1 | 2}; "onetwo"; else "other"; end; end
p alt_hval({a: 3})
p alt_hval({a: 2})

# range as an array element
def rng_elem(x); case x; in [1..5, y]; y; else :no; end; end
p rng_elem([3, 9])
p rng_elem([7, 9])

# find pattern with a literal-plus-binding capture in the window
def find_cap(xs); case xs; in [*, 3 => x, *]; x; else nil; end; end
p find_cap([1, 2, 3, 4])
p find_cap([1, 2])

# find window splats bind
def find_splats(xs)
  case xs
  in [*pre, 9, *post]; [pre, post]
  else nil
  end
end
p find_splats([1, 2, 9, 4])
p find_splats([9])

# find pattern nested inside a hash value
def find_in_hash(x); case x; in {data: [*, y, *]}; y; end; end
p find_in_hash({data: [1, 2, 3]})

# nested hash-in-hash binding
config = {a: {b: 42}}
case config
in {a: {b:}}
  p b
end

# hash-in-array element with binding
def hash_in_arr(x); case x; in [{name:}]; name; else :no; end; end
p hash_in_arr([{name: "z"}])
p hash_in_arr([3])

# nested value constraints pick the right arm
def deep(x)
  case x
  in {a: {b: 0}}; "zero"
  in {a: {b:}}; "b #{b}"
  else "none"
  end
end
p deep({a: {b: 0}})
p deep({a: {b: 7}})
p deep({a: {c: 1}})
