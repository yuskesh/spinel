# An if/else used in value position whose branches have mismatched value
# types -- a typed branch vs a void/nil branch like `$stdout.puts` -- must
# emit valid C. The puts branch is emitted as a single comma expression
# (joined value, yielding nil), not `;`-separated statements that are illegal
# inside the surrounding `return (...)`.

def w(flag, line)
  if flag
    line.length          # Integer branch
  else
    $stdout.puts(line)   # void puts in value position -> nil
  end
end

p w(true, "abc")         # 3
p w(false, "xy")         # prints "xy", then the call returns nil

# multi-argument puts (each argument on its own line) as a statement
$stdout.puts("a", "b")
