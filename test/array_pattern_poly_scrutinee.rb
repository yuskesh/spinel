# A case/in array pattern over a poly-typed scrutinee must read its length and
# elements through the poly path (sp_poly_length / sp_poly_index_poly behind a
# tag guard), never `->len` on a bare value. One helper fed several element
# kinds keeps the scrutinee poly so the typed-array fast path never applies.
def s(x); x; end

def head1?(v)
  case s(v)
  in [1, *] then "one-head"
  else "no"
  end
end
p head1?([1, 2, 3])     # "one-head"
p head1?([9, 2, 3])     # "no" -- head 9 != 1
p head1?(["a", "b"])    # "no" -- string array, head != 1
p head1?([1])           # "one-head" -- the rest may be empty

def exact?(v)
  case s(v)
  in [1, 2, 3] then "exact"
  in [1, 2, x] then "prefix #{x}"
  else "no"
  end
end
p exact?([1, 2, 3])     # "exact"
p exact?([1, 2, 9])     # "prefix 9"
p exact?(["a", "b"])    # "no"

def nested?(v)
  case s(v)
  in [1, [2, 3], 4] then "nested"
  else "no"
  end
end
p nested?([1, [2, 3], 4])   # "nested"
p nested?([1, [2, 9], 4])   # "no" -- inner trailing 9 != 3
p nested?([1, 2, 4])        # "no" -- middle element is not a sub-array

def nonarray?(v)
  case s(v)
  in [1, *] then "arr"
  else "no"
  end
end
p nonarray?(5)          # "no" -- a poly scrutinee that is not an array
p nonarray?([1, 2])     # "arr"
