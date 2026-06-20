# A call whose pointer argument is a bare variable assigned earlier in the SAME
# sequence-expression the call belongs to: the argument must be read at the call
# (after the in-sequence assignment), not hoisted above it. Regression: the arg
# was rooted into a statement-prelude temp captured before `a = {...}` ran, so
# the callee received a stale (empty) hash and every lookup blanked.
def render(h)
  h["name"]
end
def handle
  "" + ((a = {"z" => "z"}; a.delete("z"); a["name"] = "world"; render(a)))
end
puts "[" + handle + "]"

# statement form (already worked) stays correct
def handle2
  a = {"k" => "v"}
  a["name"] = "ok"
  "" + render(a)
end
puts handle2
