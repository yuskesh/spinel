# case/in against a custom #deconstruct_keys (hash pattern).
class Qt; def deconstruct_keys(k); {x: 1, y: 2}; end; end
r = case Qt.new
    in {x:, y:} then [x, y]
    end
p r
