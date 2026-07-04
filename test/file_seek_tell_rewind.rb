path = "/tmp/spinel_file_seek_test.txt"
File.open(path, "w") do |f|
  f.write("HELLOWORLDABCDEF")
end

f = File.open(path, "r")
puts f.read(4)
puts f.tell
puts f.pos
f.seek(10)
puts f.read(3)
puts f.tell
f.seek(2, IO::SEEK_CUR)
puts f.read(1)
f.seek(-6, IO::SEEK_END)
puts f.read(6)
f.seek(5, IO::SEEK_SET)
puts f.read(5)
f.rewind
puts f.tell
puts f.read(5)
f.close

File.delete(path)
puts "done"
