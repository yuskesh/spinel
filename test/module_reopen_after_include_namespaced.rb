# #573 (Sam Ruby). When a module is forward-declared empty,
# included by a class nested inside another module, and then
# reopened with method definitions, the included class's
# dispatch needs to see the late-defined methods. The bug
# required the namespaced-class shape: top-level classes
# resolved the late additions because they're collected in
# Pass 1 (after Pass 0 has already processed every top-level
# `module M; ... end` reopen), but nested-in-module classes
# get collected inside Pass 0's recursion and resolve their
# `include` against whatever module entries exist at that
# moment -- which excluded any reopen later in source order.
#
# Fix: reconcile_class_includes runs once after Pass 1.6,
# re-iterating every (class, included-module) pair so reopens
# registered after the include site finally show up.

class TestBase
end

module RequestDispatch
end

module ActionDispatch
  class IntegrationTest < TestBase
    include RequestDispatch
  end
end

module RequestDispatch
  def post(path); "post:" + path; end
end

puts ActionDispatch::IntegrationTest.new.post("/x")
