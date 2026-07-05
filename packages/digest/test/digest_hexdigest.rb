require "digest"

puts Digest::SHA256.hexdigest("hello")
puts Digest::SHA256.hexdigest("")
puts Digest::SHA1.hexdigest("hello")
puts Digest::SHA1.hexdigest("")
puts Digest::SHA256.hexdigest("hello").length
