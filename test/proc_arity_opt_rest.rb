# Proc#arity encodes a signature with an optional, rest, or post parameter as
# -(required + 1); a required-only signature stays the positive count.
p ->(a, b = 1) {}.arity
p proc { |a, *b| }.arity
p ->(a, b) {}.arity
p proc { |a| }.arity
p ->(a, *b) {}.arity
p proc { |a, b, *c| }.arity
p ->() {}.arity
p proc { |*a| }.arity
p lambda { |a, b = 1, c = 2| }.arity
p ->(a, b, c) {}.arity

# post parameters (after a rest) are required and counted
p ->(a, *b, c) {}.arity
p ->(a, *b, c, d) {}.arity

# keyword parameters: a required keyword adds a mandatory slot (stays positive);
# an optional keyword or keyword-rest alone makes the count negative.
p ->(a, b:) {}.arity
p ->(b:) {}.arity
p ->(a, b:, c:) {}.arity
p ->(a, b: 1) {}.arity
p ->(a, b:, c: 1) {}.arity
p ->(a, **k) {}.arity
p ->(a, b:, **k) {}.arity

# proc (non-lambda) forms with optional / rest
p proc { |a, b = 1| }.arity
p proc { |a, b, *c| }.arity
