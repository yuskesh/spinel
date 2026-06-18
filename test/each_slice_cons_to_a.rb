# Array#each_slice(n) / #each_cons(n) without a block return an Enumerator;
# materializing it with #to_a yields the array of slices / windows. Also the
# single-param `.map { |s| ... }` chain binds the whole slice (not an element).
p [1, 2, 3, 4, 5].each_slice(2).to_a       # [[1, 2], [3, 4], [5]]
p [1, 2, 3, 4].each_cons(2).to_a           # [[1, 2], [2, 3], [3, 4]]
p ["a", "b", "c", "d", "e"].each_slice(2).to_a

nums = [1, 2, 3, 4, 5, 6]
p nums.each_slice(3).to_a

# the single-param map chain binds the slice as a whole
p [1, 2, 3, 4, 5].each_slice(2).map { |s| s.sum }   # [3, 7, 5]
p [1, 2, 3, 4].each_cons(2).map { |w| w.sum }       # [3, 5, 7]
# many params destructure the slice into elements
p [1, 2, 3, 4].each_slice(2).map { |a, b| a * b }   # [2, 12]
