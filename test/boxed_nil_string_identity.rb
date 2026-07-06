# A NULL string is Ruby nil (the nullable-string invariant); boxing must
# preserve it, or a boxed nil-string carries the string tag and fails
# tag-keyed comparisons and nil?.
x = defined?(no_such_thing)
p x == nil
arr = [defined?(nope), 1]
p arr[0].nil?
p arr[0] == nil
h = { k: defined?(zilch) }
p h[:k].inspect
