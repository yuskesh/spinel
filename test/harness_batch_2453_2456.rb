# #2453: an array-literal aggregate in argument position (Math.log([..].max, b))
# must wrap its builder in a statement-expression, not leak it into the call.
class S
  def initialize(score); @score = score; end
  def order; Math.log([(@score + 1).abs + 2, 1].max, 10); end
end
puts S.new(5).order.round(3)

# #2455: a user module named Random must not clash with the runtime sp_Random.
module OpenSSL
  module Random
    def self.random_bytes(n)
      out = ""
      i = 0
      while i < n
        out = out + rand(256).chr
        i += 1
      end
      out
    end
  end
end
puts OpenSSL::Random.random_bytes(4).length

# #2456: Time minus a poly value (holding a Time) is a Float duration.
class A2
  def stamp; "2020-01-01"; end
end
class B2
  def stamp; Time.at(0).utc; end
end
def time_ago(time); (Time.now.utc - time).to_i > 0; end
def show(x); time_ago(x.stamp); end
puts show(B2.new)
puts (show(A2.new) rescue "raised")
