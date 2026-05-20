# `yield <typed-hash>` from a top-level method should propagate the
# hash's type to the caller's block-param local. Pre-fix
# block_param_type_at returned `int` for no-recv user yield methods
# (only the receiver-typed `arr.each` / `c.each` arms had yield-arg
# lookup wired up), so the block-local was declared `mrb_int` and
# the typed pointer was silently coerced to an int slot. Downstream
# `row["k"]` then dispatched on int and folded to 0. Issue #628.

def emit_rows
  i = 0
  while i < 2
    row = {"k" => "v" + i.to_s}
    yield row
    i += 1
  end
end

out = ""
emit_rows do |row|
  out = out + row["k"] + ","
end
puts out

# Same shape via a helper that returns the typed hash.
def make_h(s)
  {"k" => s}
end

def emit_rows_via_helper
  i = 0
  while i < 2
    yield make_h("v" + i.to_s)
    i += 1
  end
end

out2 = ""
emit_rows_via_helper do |row|
  out2 = out2 + row["k"] + ","
end
puts out2
