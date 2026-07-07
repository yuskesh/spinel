# The highest _N used is the numbered-param proc's mandatory arity.
p(-> { _1 }.arity)
p(-> { _2 }.arity)
p(-> { _1 + _3 }.arity)
p(proc { _1 }.arity)
