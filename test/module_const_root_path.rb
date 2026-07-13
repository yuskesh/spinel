# Well-known module constants reached through a root-qualified / nested path
# (::Float::MAX, ::Math::PI) infer their real type (Float) just like the plain
# Float::MAX form, so methods called on them compile instead of failing on a
# TY_UNKNOWN receiver. This matches codegen, which already resolves the nested
# parent path.
p(::Float::MAX > 1.0)
p(::Math::PI.round(2))
p(::Math::E.round(3))
p(::Float::INFINITY.infinite?)
