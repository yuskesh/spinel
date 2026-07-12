# Exception value semantics: #== by class+message, KeyError key inspect,
# Math::DomainError naming, private/public NameError on undefined names,
# NameError#name, and raise with an explicit cause.

# == compares class and message; equal? stays identity
def pair_eq(a, b); a == b; end
p pair_eq(TypeError.new("m"), TypeError.new("m"))
p pair_eq(TypeError.new("m"), TypeError.new("other"))
p pair_eq(TypeError.new("m"), ArgumentError.new("m"))
e1 = RuntimeError.new("same")
p pair_eq(e1, e1)
p e1.equal?(RuntimeError.new("same"))

# KeyError message renders a symbol key as :sym
def dig(h, k); h.fetch(k); end
begin; dig({}, :missing_key); rescue KeyError => e; puts e.message; end
begin; dig({ a: 1 }, :b); rescue KeyError => e; puts e.message; end

# fetch on an empty literal still finds present keys after insert
h2 = {}
h2[:x] = 5
p dig(h2, :x)

# Math::DomainError renders qualified and rescues by both spellings
def bad_sqrt(x) = Math.sqrt(x)
begin
  bad_sqrt(-1.0)
rescue Math::DomainError => e
  p e.class
  puts e.message
end

# private with an undefined name raises NameError when the body runs
class Vis
  def real; end
  begin
    private :absent
    puts "no raise"
  rescue NameError => e
    puts e.class
    puts e.message.lines.first
  end
  private :real
end
p Vis.new.respond_to?(:real)

# NameError#name carries the missing name; other classes raise NoMethodError
def build_ne(m, n); NameError.new(m, n).name; end
p build_ne("msg", :foo)
begin
  TypeError.new("m").name
rescue NoMethodError => e
  puts e.class
end

# raise with an explicit cause: kwarg overrides the implicit chain
def blow(m); raise "top", cause: ArgumentError.new(m); end
begin
  blow("root")
rescue => e
  p e.cause
  puts e.message
end

# implicit cause still threads when no kwarg is given
begin
  begin
    raise ArgumentError, "inner"
  rescue ArgumentError
    raise "outer"
  end
rescue => e
  p e.cause
end
