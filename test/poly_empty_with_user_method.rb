# Defining an instance `empty?` must not drop the builtin String/Array `empty?`
# from the polymorphic `empty?` dispatch: a poly receiver was lowered to a
# cls_id switch built only from user `empty?` methods, so a String (or Array)
# receiver fell through and `empty?` returned false. Sibling of #1437. The
# dispatch must also carry the builtin tag/cls_id arms.
class Flasher
  def empty?
    false
  end
end

def pick(c)
  c ? "" : "x"
end
def pickarr(c)
  c ? [] : [1]
end

puts pick(true).empty?     # true
puts pick(false).empty?    # false
puts pickarr(true).empty?  # true
puts pickarr(false).empty? # false
puts Flasher.new.empty?    # false  (user method still dispatches)
