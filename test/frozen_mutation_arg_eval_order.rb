# Ruby evaluates a mutator's arguments before the call runs, so a
# side-effecting argument (and an op-assign RHS) must still execute even when
# the receiver is frozen and the mutation ultimately raises FrozenError. Each
# "arg:X" must print BEFORE its "... raised" line.
def note(x); puts "arg:#{x}"; x; end

begin; "hi".freeze.concat(note("!")); rescue FrozenError; puts "concat raised"; end
begin; "yo".freeze.prepend(note(">")); rescue FrozenError; puts "prepend raised"; end

h = {n: 1}.freeze
begin; h[:n] += note(9); rescue FrozenError; puts "hash raised"; end

# sanity: the unfrozen path still evaluates the arg AND mutates
m = "go"; m << note("!"); puts m
