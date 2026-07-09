# A user exception's #message / #to_s override must drive the reported message.
# Ruby's Exception#message calls #to_s, so overriding either changes #message,
# while overriding only #message leaves #to_s at the default (the class name).

class MsgOnly < StandardError
  def message = "msg-only"
end

class TosOnly < StandardError
  def to_s = "tos-only"
end

class Both < StandardError
  def message = "the-message"
  def to_s = "the-to-s"
end

class WithIvar < StandardError
  def initialize(n)
    @n = n
    super("stored")
  end
  def message = "n=#{@n}"
end

class Plain < StandardError
end

def show(e)
  puts "message=#{e.message} to_s=#{e.to_s}"
end

begin; raise MsgOnly; rescue => e; show(e); end
begin; raise TosOnly; rescue => e; show(e); end
begin; raise Both; rescue => e; show(e); end
begin; raise Both, "ignored-arg"; rescue => e; show(e); end
begin; raise WithIvar.new(7); rescue => e; show(e); end
begin; raise Plain, "explicit"; rescue => e; show(e); end
begin; raise Plain; rescue => e; show(e); end

# specialized rescue (statically-typed binding) hits the override too
begin; raise Both; rescue Both => e; show(e); end
