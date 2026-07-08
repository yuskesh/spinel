# Dir.glob with a recursive `**` component walks the subtree; the text before
# `**` is preserved in each result and a cwd-anchored pattern yields bare
# relative paths, sorted (Ruby 3.0+). Uses a cwd-relative tree so it works
# anywhere, and removes everything it creates.
root = "sptest_glob_rec"
Dir.mkdir(root) unless Dir.exist?(root)
Dir.mkdir("#{root}/a") unless Dir.exist?("#{root}/a")
Dir.mkdir("#{root}/a/b") unless Dir.exist?("#{root}/a/b")
File.write("#{root}/x.rbs", "")
File.write("#{root}/a/y.rbs", "")
File.write("#{root}/a/b/z.rbs", "")
File.write("#{root}/a/keep.txt", "")

puts Dir.glob("#{root}/**/*.rbs").join(",")   # all three, prefixed with root
puts Dir.glob("#{root}/**/*.rbs").length       # 3
puts Dir.glob("#{root}/*.rbs").join(",")       # only x.rbs (single level)
puts Dir.glob("#{root}/**/*.txt").join(",")    # keep.txt

File.delete("#{root}/x.rbs")
File.delete("#{root}/a/y.rbs")
File.delete("#{root}/a/b/z.rbs")
File.delete("#{root}/a/keep.txt")
Dir.rmdir("#{root}/a/b")
Dir.rmdir("#{root}/a")
Dir.rmdir(root)
