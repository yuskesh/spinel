# Method visibility tracking: private / protected / public affect the
# reflection (instance_methods family, method_defined? family) and respond_to?.

class Account
  def deposit(n); n; end       # public (default)
  def balance; 100; end        # public

  private

  def log(msg); msg; end       # private (section)
  def secret; 42; end          # private (section)

  protected

  def compare(o); o; end       # protected (section)

  public

  def withdraw(n); n; end      # public (re-opened section)

  private :balance             # demote a previously-public method
  private def helper; 1; end   # inline private def
end

def sorted(arr); arr.map(&:to_s).sort; end

# instance_methods(false) excludes private, keeps public + protected
puts sorted(Account.instance_methods(false)).inspect
puts sorted(Account.public_instance_methods(false)).inspect
puts sorted(Account.private_instance_methods(false)).inspect
puts sorted(Account.protected_instance_methods(false)).inspect

# method_defined? matches public + protected, not private
puts Account.method_defined?(:deposit)
puts Account.method_defined?(:compare)
puts Account.method_defined?(:log)
puts Account.method_defined?(:balance)     # demoted to private
puts Account.method_defined?(:helper)
puts Account.method_defined?(:missing)

# the prefixed forms match exactly one visibility
puts Account.public_method_defined?(:deposit)
puts Account.public_method_defined?(:log)
puts Account.private_method_defined?(:log)
puts Account.private_method_defined?(:helper)
puts Account.protected_method_defined?(:compare)
puts Account.protected_method_defined?(:deposit)

# respond_to?: public only by default; include_all=true also matches private/protected
a = Account.new
puts a.respond_to?(:deposit)
puts a.respond_to?(:log)
puts a.respond_to?(:log, true)
puts a.respond_to?(:secret)
puts a.respond_to?(:secret, true)
puts a.respond_to?(:compare)
puts a.respond_to?(:compare, true)
puts a.respond_to?(:withdraw)
puts a.respond_to?(:nope)
puts a.respond_to?(:nope, true)
