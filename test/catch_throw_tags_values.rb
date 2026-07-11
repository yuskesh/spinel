# catch/throw: values delivered across method boundaries, non-symbol object
# tags matched by identity, the auto-tag block-param form, and UncaughtThrowError.

# throw value crosses a method boundary
def thrower; throw :done, [1, 2]; end
def catcher; catch(:done) { thrower }; end
p catcher

# non-array value across methods
def deep_throw; throw :msg, "carried"; end
def deep_catch; catch(:msg) { deep_throw; "not reached" }; end
p deep_catch

# object tag: matched by identity
tag = Object.new
r = catch(tag) { throw tag, 7 }
p r

# two distinct object tags: inner throw targets the outer catch
outer = Object.new
inner = Object.new
r2 = catch(outer) do
  catch(inner) do
    throw outer, :skipped_inner
  end
  :not_reached
end
p r2

# auto-generated tag bound to the block param
def auto; catch { |t| throw t, 42 }; end
p auto

# auto-tag without a throw returns the block value
p(catch { |t| 5 })

# no throw: catch returns the block's last value
p(catch(:unused) { "fallthrough" })

# throw with no value delivers nil
p(catch(:bare) { throw :bare; :after })

# uncaught throw raises UncaughtThrowError
begin
  catch(:here) { throw :elsewhere, 1 }
rescue UncaughtThrowError => e
  puts e.class
  puts e.message
end

# int values still ride the channel
p(catch(:n) { throw :n, 99 })

# float value
p(catch(:f) { throw :f, 2.5 })

# an Integer TAG matches by identity (Fixnum value equality), not content
p(catch(42) { throw 42, "int-tag" })
p(catch(7) { 99 })   # no throw: the block value rides through
