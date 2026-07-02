# frozen_string_literal: true
def frozen_helper_lit
  "helper frozen lit"
end

def frozen_helper_mutate
  s = "helper buf"
  s << "y"   # FrozenError: the literal comes from a pragma file
end
