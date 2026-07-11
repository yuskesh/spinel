# Constant ||= typing, toplevel ivar op-assign, defined?(implicit method),
# and ENV parity: fetch KeyError, nil-deletes, non-String TypeError, size.

# `CONST ||= v` as the only definition types the constant
LIMIT ||= 10
puts LIMIT + 1

# toplevel ivar compound assignment
@count = 0
@count += 1
@count *= 5
p @count

# defined? sees an implicit-self method inside a class
class Widget
  def name = "w"
  attr_reader :size
  def check
    [defined?(name), defined?(size), defined?(missing_thing)]
  end
end
p Widget.new.check

# ENV.fetch with a default and with a KeyError
ENV["SPINEL_PARITY_A"] = "set"
p ENV.fetch("SPINEL_PARITY_A")
p ENV.fetch("SPINEL_PARITY_MISSING", "fallback")
begin
  ENV.fetch("SPINEL_PARITY_MISSING")
rescue KeyError => e
  puts e.class
  puts e.message.lines.first
end

# ENV[k] = nil through a runtime value deletes
def env_set(k, v); ENV[k] = v; end
env_set("SPINEL_PARITY_A", nil)
p ENV["SPINEL_PARITY_A"]

# literal nil deletes too
ENV["SPINEL_PARITY_B"] = "x"
ENV["SPINEL_PARITY_B"] = nil
p ENV["SPINEL_PARITY_B"]

# non-String RHS raises TypeError naming the class
def env_set_poly(v); ENV["SPINEL_PARITY_C"] = v; end
vals = [7, "ok"]
begin
  env_set_poly(vals[0])
rescue TypeError => e
  puts "#{e.class}: #{e.message}"
end
env_set_poly(vals[1])
p ENV["SPINEL_PARITY_C"]

# ENV.size counts the environment
p ENV.size > 0
p ENV.length == ENV.size
