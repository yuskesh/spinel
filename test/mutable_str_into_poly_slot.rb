# #541. A view function returning sp_String * boxed into a
# `body` slot that had been widened to poly (other classes wrote
# ints/etc. to the same-named slot) emitted
# `sp_box_obj((void *)val, 0)` -- a generic SP_TAG_OBJ box with
# cls_id 0 (BasicObject). Downstream code that dispatches on
# the body's tag/cls_id (e.g. Tep::Server#write_response in
# roundhouse, computing Content-Length and writing the wire)
# had no arm matching SP_TAG_OBJ + cls_id 0, so the body was
# silently dropped -- HTTP 200 with empty body in real-blog.
#
# Fix: `box_non_nullable_value_to_poly`'s mutable_str arm now
# routes through `sp_box_str(s->data)` (SP_TAG_STR with the
# underlying char* buffer) instead of the catch-all
# `sp_box_obj((void *)..., 0)`. Consumers reading `v.v.s` on
# the tag find the string content.

class View
  def self.index
    s = String.new
    s << "<h1>title</h1>"
    s << "<p>body</p>"
    s
  end
end

class Resp
  attr_accessor :body
  def initialize; @body = ""; end
end

class OtherResp
  attr_accessor :body
  def initialize; @body = 42; end  # widens body across classes to poly
end

r = Resp.new
r.body = View.index
# Without the fix: body boxed as sp_box_obj((void *), 0); .inspect
# dispatches via sp_poly_inspect which falls into the SP_TAG_OBJ
# case but cls_id 0 matches no arm; result is generic /
# truncated. With the fix: body boxed as SP_TAG_STR; inspect
# returns the quoted string.
puts r.body.inspect

# Trigger the OtherResp widening so the analyzer sees both
# int and string writes; verifies the str arm still works under
# pre-widened body slots.
o = OtherResp.new
puts o.body.inspect

r2 = Resp.new
r2.body = View.index
puts r2.body.inspect
