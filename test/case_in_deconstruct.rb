# case/in against a custom #deconstruct (array pattern).
class Pt; def deconstruct; [1, 2]; end; end
r = case Pt.new
    in [a, b] then [a, b]
    end
p r
