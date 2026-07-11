# A constant-path receiver whose LEAF is a module/class alias resolves to
# the aliased target, like the simple-constant receiver form already does.
module Outer
  module Util
    def self.five = 5
    def self.echo(s) = s
  end
  OC = Util
end

p Outer::OC.five
p Outer::OC.echo("hi")
p Outer::Util.five
X = Outer::Util
p X.five
