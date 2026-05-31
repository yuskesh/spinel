# Regression for #1150 bug 1: `require "x"; code` on one line truncated the
# trailing code (the whole line was replaced). The code after the require
# must survive.
puts "start"
require "json"; puts JSON.generate([1, 2, 3])
