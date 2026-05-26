# Issue #892: File.extname / .basename / .dirname / Dir.pwd.
puts File.extname("hello.rb")
puts File.basename("/home/user/file.rb")
puts File.dirname("/home/user/file.rb")
# Edge cases.
puts File.extname(".bashrc")       # "" — leading-dot only
puts File.extname("foo.")          # ""
puts File.dirname("foo")           # "."
puts File.dirname("/usr/local")    # "/usr"
# Dir.pwd returns a string.
puts Dir.pwd.length > 0
