# A bare empty-hash literal as a receiver coerces to the same str-keyed poly
# hash the emitter builds for {}, so direct calls dispatch.
p({}.size)
p({}.empty?)
p({} == {})
p [].size
