# Forwarding `&block` into a callee that nil-checks the forwarded block.
# The combination (forward + nil-check) previously rejected at compile time.

def inner(&blk) = (blk ? blk.call : 0)
def outer(&blk) = inner(&blk)

puts outer { 7 }          # block forwarded through: 7
puts outer                # no block given: forwarded nil, inner's check -> 0

def wrap(&b) = inner(&b)
puts wrap { 6 * 7 }       # value flows through the forwarded block: 42

# anonymous block forwarding (`&`)
def anon(&) = inner(&)
puts anon { 5 }           # 5
puts anon                 # 0

# receiver-form: an instance method forwarding into a nil-checking sibling
# (distinct method names to avoid an unrelated implicit-self resolution quirk)
class Runner
  def run_inner(&blk) = (blk ? blk.call : -1)
  def run_outer(&blk) = run_inner(&blk)
end
r = Runner.new
puts r.run_outer { 8 }    # 8
puts r.run_outer          # nil forwarded -> -1
