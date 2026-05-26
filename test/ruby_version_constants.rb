# Issue #890: RUBY_VERSION / RUBY_PLATFORM / RUBY_ENGINE built-in
# string constants. Format-only check so the test stays stable
# across platforms — the actual values are runtime-detected.
puts RUBY_VERSION.is_a?(String)
puts RUBY_PLATFORM.is_a?(String)
puts RUBY_ENGINE.is_a?(String)
puts RUBY_PLATFORM.length > 0
