require "base64"

# The three CRuby entry-point pairs. encode64 wraps at 60 columns and ends
# with a newline; strict is unwrapped; urlsafe uses the -_ alphabet (padded).
puts Base64.encode64("hello world")
puts Base64.strict_encode64("hello world")
puts Base64.urlsafe_encode64("a+b/c?d~e")
puts Base64.decode64(Base64.encode64("round trip!"))
puts Base64.strict_decode64("aGVsbG8=")
puts Base64.urlsafe_decode64(Base64.urlsafe_encode64("x~y"))

# 60-column wrapping on a long input.
print Base64.encode64("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor")

# decode64 skips whitespace (the wrapped form round-trips).
long = "abcdefghijklmnopqrstuvwxyz" * 5
puts Base64.decode64(Base64.encode64(long)) == long
