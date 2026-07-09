# Keyword args through the instance-method dispatch path: a `**kwrest` callee
# collects literal keywords and merged `**hash` forwards; a named keyword
# param extracts from a forwarded `**hash` (absent key -> default); a
# positional param whose NAME matches a forwarded key still takes the
# provided positional (it never steals the key). Same rules emit_args_filled
# applies to free-function calls.
class Sink
  def note_mutation(op, node, **info)
    puts "#{op}:#{node}:#{info[:parent]}:#{info[:index]}:#{info[:key]}"
  end

  def named(op, parent: "none", index: 0)
    puts "named #{op}:#{parent}:#{index}"
  end

  def note(op, node, **info)
    note_mutation(op, node, **info)
  end

  def relay(op, **info)
    named(op, **info)
  end
end

s = Sink.new
s.note_mutation(:ext, "n0", key: "href")
s.note(:fwd, "n1", parent: "P", index: 3)
s.note(:bare, "n2")
s.relay(:r1, parent: "Q")
s.relay(:r2)

# a positional param named like a forwarded key takes the positional
class Pos
  def take(parent, **info)
    puts "pos #{parent}:#{info[:parent]}"
  end
  def go(**info) = take("given", **info)
end
Pos.new.go(parent: "fromkw")
