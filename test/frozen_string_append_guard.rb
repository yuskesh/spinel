# <<, concat, and prepend on a frozen String raise FrozenError with the CRuby
# message (including contents), whether the receiver is a chained .freeze or a
# frozen local; a mutable string appends normally.
def ap(s, x); s << x; end
begin; ap("abc".freeze, "x"); rescue FrozenError => e; puts e.message; end
begin; "hi".freeze.concat("!"); rescue FrozenError => e; puts e.message; end
begin; "yo".freeze.prepend(">"); rescue FrozenError => e; puts e.message; end
d = "abc".dup.freeze
begin; d << "z"; rescue FrozenError => e; puts e.message; end
m = "go"; m << "!"; puts m
