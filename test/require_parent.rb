# Regression: `require_relative ".."` must load the parent directory's
# `<dir>.rb`. Ruby normalizes "views/articles/.." -> "views" and appends
# ".rb" -> "views.rb"; Spinel must not glue ".rb" onto a literal ".." to
# form a bogus "...rb". (Mirrors roundhouse's emitted views, where each
# view does `require_relative ".."` to pull in the views aggregator.)
require_relative "require_parent/views/articles/index"
