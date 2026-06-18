# `flat_map { |x| x }` over an array of arrays is a one-level flatten (it is
# exactly `flatten(1)`): each sub-array is concatenated and scalars pass through.
# Distinct per-type helpers keep each nested element type monomorphic; receivers
# route through a method param to defeat constant folding.
def nii(x) = x
def nss(x) = x
def nff(x) = x
def nmix(x) = x
def npass(x) = x
def ints(x) = x

p nii([[1, 2], [3, 4], [5]]).flat_map { |a| a }      # [1, 2, 3, 4, 5]
p nss([["a", "b"], ["c"]]).flat_map { |b| b }         # ["a", "b", "c"]
p nff([[1.5, 2.5], [3.5]]).flat_map { |c| c }         # [1.5, 2.5, 3.5]
p npass([[1, 2], 3, [4]]).flat_map { |d| d }          # [1, 2, 3, 4] (scalar passthrough)
p nmix([[1, "a"], [2, "b"]]).flat_map { |e| e }       # [1, "a", 2, "b"]

# one level only: a deeper sub-array is left intact (matches flatten(1))
p nmix([[1, [2]], [3]]).flat_map { |f| f }            # [1, [2], 3]

# assigned result and a chained method
r = nii([[10], [20, 30]]).flat_map { |g| g }
p r                                                  # [10, 20, 30]
p nii([[1, 2], [3, 4]]).flat_map { |h| h }.length     # 4

# a non-identity (array-returning) flat_map is unchanged
p ints([1, 2, 3]).flat_map { |i| [i, i * 10] }        # [1, 10, 2, 20, 3, 30]
