# A user class owning `message` / `backtrace` / `cause` / `full_message` must not
# take those attributes away from a builtin exception that reaches the same poly
# dispatch.
#
# `e.message` on a poly-typed receiver lowers to a switch on the boxed value's
# cls_id with one case per user class that defines the name.  A boxed builtin
# exception (cls_id SP_BUILTIN_EXCEPTION) has no case of its own, and the
# switch's default arm raised NoMethodError -- so an ordinary
# `RuntimeError#message` stopped answering as soon as ANY class in the program
# happened to define `message`.  Nothing here is exotic: a class, a helper and
# two calls is the whole shape.
class MsgOnly < StandardError
  def message = "msg-only"
end
class Both < StandardError
  def message = "the-message"
  def to_s = "the-to-s"
end

def show_message(e)
  puts e.message
end

show_message(MsgOnly.new)
show_message(Both.new)
show_message(RuntimeError.new("boom"))
show_message(StandardError.new("plain"))
show_message(ArgumentError.new("bad arg"))

begin
  raise "raised"
rescue => e
  show_message(e)
end
[MsgOnly.new, RuntimeError.new("in-array")].each { |x| puts x.message }

# the same for the other three attributes the poly runtime serves
class BtUser < StandardError
  def backtrace = ["user-bt"]
end
def show_backtrace(e)
  p e.backtrace
end
show_backtrace(BtUser.new)
show_backtrace(RuntimeError.new("no-frames"))

class CauseUser < StandardError
  def cause = RuntimeError.new("user-cause")
end
def show_cause(e)
  c = e.cause
  puts(c.nil? ? "nil" : c.message)
end
show_cause(CauseUser.new)
show_cause(RuntimeError.new("no-cause"))

class FullUser < StandardError
  def full_message = "user-full"
end
# CRuby's #full_message embeds the backtrace and spinel records no frames, so
# the two render differently BOTH with and without a user class owning the name
# (a pre-existing divergence this fix does not touch).  Assert the part that is
# portable: it answers, and it carries the message.
def show_full(e)
  puts e.full_message.include?("boom2") || e.full_message.include?("user-full")
end
show_full(FullUser.new)
show_full(RuntimeError.new("boom2"))

# the B9 shape: two named rescue arms split apart, one specialised to a user
# class and one left as a plain exception, both handed to one helper
class AErr < StandardError
  def message = "aaa"
end
class BErr < StandardError
end
def raiser(n)
  raise AErr if n == 1
  raise BErr if n == 2
  0
end
begin; raiser(1); rescue AErr => x; show_message(x); end
begin; raiser(2); rescue BErr => x; show_message(x); end
