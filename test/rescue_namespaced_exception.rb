# A user exception raised under its qualified Ruby name ("App::Error") must be
# matched by every arm shape: a namespaced arm (`rescue App::Error`, whose AST
# name is the leaf), a leaf-referenced arm inside the module, a parent-class
# arm catching a namespaced subclass (the hierarchy callback registers
# qualified parents), and a builtin StandardError arm. Covers both the
# class-fallback raise (sp_raise_cls) and the constructed-object raise.
module App
  class Error < StandardError
  end

  class SubError < Error
  end

  def self.leafy
    raise Error, "leaf ref"
  rescue Error => e
    puts "leaf caught: #{e.message}"
  end
end

begin
  raise App::SubError, "sub raise"
rescue App::Error => e
  puts "parent arm caught: #{e.message}"
end

App.leafy

begin
  raise App::Error.new("explicit new")
rescue StandardError => e
  puts "std caught: #{e.message}"
end
