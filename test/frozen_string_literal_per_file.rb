# Per-file frozen_string_literal: each literal carries its OWN file's pragma,
# not the entry file's. helper_frozen.rb has the pragma; this entry file and
# helper_plain.rb do not.
require_relative "frozen_string_literal_per_file/helper_frozen"
require_relative "frozen_string_literal_per_file/helper_plain"

p frozen_helper_lit.frozen?   # true  -- literal from the pragma file
p plain_helper_lit.frozen?    # false -- literal from a plain file
p "entry lit".frozen?         # false -- entry has no pragma

# mutating a pragma-file literal raises even when called from a plain file
begin
  frozen_helper_mutate
  puts "BUG: no raise"
rescue FrozenError => e
  puts e.message
end

# a plain-file literal stays mutable
p plain_helper_build
