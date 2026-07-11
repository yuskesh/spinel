# Spinel bundled `base64` — a carried-C spin package (Path B).
#
# The whole implementation lives in this package's C (sp_base64.c, linked only
# when `require "base64"` appears); the declarations below are the compiler's
# entire knowledge of it. Binary-safe: lengths ride the string header on both
# sides.
module Base64
  native_lib "base64"
  native_obj "packages/base64/sp_base64.o"
  native_func :encode64,         [:string], :string, "sp_base64_encode64"
  native_func :decode64,         [:string], :string, "sp_base64_decode64"
  native_func :strict_encode64,  [:string], :string, "sp_base64_strict_encode64"
  native_func :strict_decode64,  [:string], :string, "sp_base64_strict_decode64"
  native_func :urlsafe_encode64_padded, [:string], :string, "sp_base64_urlsafe_encode64"
  native_func :urlsafe_decode64, [:string], :string, "sp_base64_urlsafe_decode64"

  # urlsafe_encode64 honors padding: false by stripping the trailing '='
  # fill (the only place '=' occurs in base64 output).
  def self.urlsafe_encode64(bin, padding: true)
    s = Base64.urlsafe_encode64_padded(bin)
    padding ? s : s.delete("=")
  end
end
