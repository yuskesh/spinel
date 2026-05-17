# `(Fiber[:k] = v)` as an expression returns the assigned value,
# matching MRI's `(h[k]=v) => v` semantics. The codegen path is
# distinct from the statement form `Fiber[:k] = v` — that lands in
# compile_mutating_call_stmt, while the expression-context dispatch
# emits a gcc statement-expression so the RHS reaches the outer
# assignment.

x = (Fiber[:n] = 42)
puts x                # 42 — value of the assignment expression
puts Fiber[:n]        # 42 — confirms the write also took effect

# Chained: `Fiber[:a] = Fiber[:b] = 7`. Right-to-left associativity
# makes `Fiber[:b] = 7` evaluate first (yielding 7), then
# `Fiber[:a] = 7`.
Fiber[:a] = Fiber[:b] = 7
puts Fiber[:a]
puts Fiber[:b]
