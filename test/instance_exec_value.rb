# A top-level method forwarding a block to instance_exec, with its return value
# consumed. Previously the value form linked against an un-emitted `sp_run`; a
# receiverless instance_exec does not rebind self, so it is exactly `b.call`.

def run(&b) = instance_exec(&b)

# value form (the reported failure)
x = run { 42 }
puts x

# side-effect form
run { puts "side-effect" }

# value used inside a larger expression
def compute(&b) = instance_exec(&b)
puts(compute { 10 } + compute { 20 })

# positional args forward to the block
def run_args(n, &b) = instance_exec(n, &b)
puts run_args(21) { |v| v * 2 }

# a captured local is visible (self is unchanged at top level)
factor = 3
def apply(&b) = instance_exec(&b)
puts apply { factor * 10 }

# class-level instance_exec still rebinds self to the instance (unchanged path)
class Widget
  def initialize = (@n = 7)
  def tap_it(&b) = instance_exec(&b)
end
puts Widget.new.tap_it { @n * 2 }
