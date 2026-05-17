begin
  File.write(".", "hello")
  puts "write succeeded"
rescue => e
  puts e.class
  puts e.message.include?("Is a directory")
end
