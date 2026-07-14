filename = "test_unlink_rename_temp.txt"
renamed = "test_unlink_rename_temp2.txt"
File.write(filename, "file op test")
File.rename(filename, renamed)
puts File.exist?(filename)
puts File.exist?(renamed)
File.unlink(renamed)
puts File.exist?(renamed)
