# A top-level `@ivar ||=` / `@ivar &&=` must emit its store. Previously the
# top-level slot was unresolved (class_id < 0), so no branch matched and the
# assignment was silently dropped.
@y ||= 5
p @y                 # 5
@y ||= 99
p @y                 # 5  (already set, ||= is a no-op)

@s ||= "hi"
p @s                 # "hi"

@a = 3
@a &&= 10
p @a                 # 10
@b &&= 10
p @b                 # nil (unset stays nil under &&=)
