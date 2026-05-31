# Regression for #1150 bug 2: a `require` on the very first line (byte 0)
# was skipped when another `require` followed it, so optparse.rb was never
# inlined and OptionParser was undefined. Both requires must be processed.
require "optparse"
require "tmpdir"
o = OptionParser.new
puts o.class
