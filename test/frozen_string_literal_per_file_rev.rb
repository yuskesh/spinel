# frozen_string_literal: true
# Reverse direction: pragma entry + plain helper. The helper's literals stay
# unfrozen and mutable; the entry's literals are frozen.
require_relative "frozen_string_literal_per_file/helper_plain"

p "entry lit".frozen?         # true
p plain_helper_lit.frozen?    # false
p plain_helper_build          # helper literals stay mutable
