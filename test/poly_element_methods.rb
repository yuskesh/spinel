# Array-reduction methods (sum / min / max / first / last / length) called on the
# elements of a poly array whose elements are themselves arrays -- e.g. the runs
# produced by chunk_while. The element type is erased to poly, so the method is
# dispatched at runtime by the boxed element's class. Works through an explicit
# block and through &:sym. Distinct per-type helpers keep each run monomorphic.
def ii(x) = x
def nff(x) = x
def nss(x) = x

runs = ii([1, 2, 4, 5, 7]).chunk_while { |a, b| b - a == 1 }.to_a  # [[1,2],[4,5],[7]]
p runs.map { |r| r.sum }                                          # [3, 9, 7]
p runs.map(&:sum)                                                 # [3, 9, 7]
p runs.map { |r| r.length }                                       # [2, 2, 1]
p runs.map(&:max)                                                 # [2, 5, 7]
p runs.map { |r| r.min }                                          # [1, 4, 7]
p runs.map { |r| r.first }                                        # [1, 4, 7]
p runs.map(&:last)                                                # [2, 5, 7]
runs.each { |r| p r.sum }                                         # 3 / 9 / 7
p runs.select { |r| r.sum > 5 }                                   # [[4, 5], [7]]

# float runs sum/max to floats
fr = nff([[1.5, 2.5], [3.5]])
p fr.map { |r| r.sum }                                            # [4.0, 3.5]
p fr.map(&:max)                                                   # [2.5, 3.5]

# string runs: first / last / length
sr = nss([["a", "b"], ["c"]])
p sr.map { |r| r.first }                                          # ["a", "c"]
p sr.map(&:length)                                                # [2, 1]

# direct poly-array methods are unaffected (length / first of the runs array)
p runs.length                                                     # 3
p runs.first                                                      # [1, 2]
