# A <=> returning nil makes the ordering comparisons raise ArgumentError,
# while Comparable#== instead answers false rather than raising.
class Bad
  include Comparable
  def <=>(o); nil; end
end
begin; Bad.new < Bad.new;  rescue => e; p e.class; p e.message; end
begin; Bad.new > Bad.new;  rescue => e; p e.class; p e.message; end
begin; Bad.new <= Bad.new; rescue => e; p e.class; p e.message; end
begin; Bad.new >= Bad.new; rescue => e; p e.class; p e.message; end
p(Bad.new == Bad.new)
