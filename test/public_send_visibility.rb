# public_send dispatches only public methods: a private or protected target
# raises NoMethodError with CRuby's message, while send/__send__ stay
# visibility-blind.
def err
  yield
rescue NoMethodError => e
  "#{e.class}: #{e.message}"
end

class Acct
  def balance = 100
  def show(label) = "#{label}: #{balance}"

  def peer_check(other) = other.guarded

  protected

  def guarded = "prot"

  private

  def secret = 42
end

a = Acct.new
p a.public_send(:balance)
p a.public_send(:show, "acct")
p a.public_send("balance")
puts err { a.public_send(:secret) }
puts err { a.public_send("secret") }
puts err { a.public_send(:guarded) }

# send / __send__ ignore visibility
p a.send(:secret)
p a.__send__(:secret)

# protected still callable through peer dispatch
p a.peer_check(Acct.new)

# receiverless public_send of a private method raises too
class Inner
  def run = public_send(:hidden)
  private
  def hidden = 1
end
puts err { Inner.new.run }

# runtime-computed name: visibility enforced per resolved arm
def dyn(o, m) = o.public_send(m)
p dyn(a, :balance)
puts err { dyn(a, :secret) }
