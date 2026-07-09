# An exception subclass with ivars must reserve the StopIteration#result slot in
# its generated struct (mirroring sp_Exception's cls_name/parent_cls_name/msg/
# cause/result prefix). Without it `result` -- being wider than a pointer --
# aliases and overruns the subclass's first ivar: a heap overflow in the
# constructor's `result = nil` write and the GC scan's mark, and #result then
# reads the ivar's bits instead of nil. A StopIteration subclass carrying an
# ivar therefore reports nil #result (it was fed no iteration value): non-nil
# would mean `result` overlaps @limit; 0 would mean the slot was never nil-init.
class BoundedStop < StopIteration
  def initialize(limit)
    super("stop")
    @limit = limit
  end
end

begin
  raise BoundedStop.new(7)
rescue StopIteration => e
  p e.result
end

# holds across repeated allocation churn (GC may scan live subclass exceptions)
ok = true
50.times do |i|
  begin
    raise BoundedStop.new(i)
  rescue StopIteration => e
    ok = false unless e.result.nil?
  end
end
p ok
