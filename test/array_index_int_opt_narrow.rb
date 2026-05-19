# Array#index on int_array returns the new int? (scalar-nullable
# int with INT64_MIN sentinel) instead of being widened to poly.
# After the nil-check guard the value narrows back to plain int
# so downstream arithmetic compiles to direct integer ops without
# poly box/unbox.
#
# Sibling of nil_guard_narrows_int_or_nil.rb (which exercises the
# same shape on String#index, still poly-widened). Both forms
# behave identically at the Ruby level; this test pins the
# int_array case so a regression in the int?-narrow path surfaces
# here.

ARR = [10, 20, 30, 40, 50]

def offset_or_zero(arr, target)
  i = arr.index(target)
  return 0 if i.nil?
  i + 1
end

puts offset_or_zero(ARR, 30)    # 3
puts offset_or_zero(ARR, 999)   # 0
puts offset_or_zero(ARR, 10)    # 1

# Inspect/.nil? on the int? value flows through the sentinel-aware
# helpers (sp_int_opt_inspect / sp_int_is_nil).
puts ARR.index(20).inspect      # 1
puts ARR.index(999).inspect     # nil
puts ARR.index(999).nil?        # true
puts ARR.index(20).nil?         # false
