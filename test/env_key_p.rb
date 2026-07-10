# ENV.key? / has_key? / include? / member? -> getenv presence (bool).
# Previously an unresolved call ("undefined method 'key?' for unknown").
p ENV.key?("PATH")
p ENV.key?("SPINEL_NO_SUCH_VAR_12345")
ENV["SPINEL_ENV_KEY_TEST"] = "1"
p ENV.has_key?("SPINEL_ENV_KEY_TEST")
p ENV.include?("SPINEL_ENV_KEY_TEST")
p ENV.member?("SPINEL_ENV_KEY_TEST")
ENV["SPINEL_ENV_KEY_TEST"] = nil
p ENV.key?("SPINEL_ENV_KEY_TEST")
minimal = ENV.key?("MINIMAL_NO_SUCH")
p minimal ? 1 : 0
