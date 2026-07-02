# Array#[]= splice exceptional cases: exact CRuby exception class + message.

# negative length raises IndexError
begin
  a = [1, 2, 3, 4]
  a[1, -2] = [9]
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end

# negative length via the start,len scalar form too
begin
  a = [1, 2, 3]
  a[0, -1] = 5
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end

# splicing into a frozen array raises FrozenError
begin
  a = [1, 2, 3].freeze
  a[0, 1] = [9]
rescue FrozenError => e
  puts e.class
end

# a start before the front raises IndexError
begin
  a = [1, 2, 3]
  a[-9, 1] = [0]
rescue IndexError => e
  puts e.class
end

# poly receiver (an element that is a typed array at runtime): same exceptions.
# negative length
begin
  a = [[1, 2, 3], "x"]
  a[0][1, -1] = [9]
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end

# start before the front
begin
  a = [[1, 2, 3], "x"]
  a[0][-5, 1] = [9]
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end

# a range whose begin is before the front raises RangeError (not IndexError)
begin
  a = [[1, 2, 3], "x"]
  a[0][-10..1] = [9]
rescue RangeError => e
  puts "#{e.class}: #{e.message}"
end

# an endless range whose begin is before the front raises RangeError too
begin
  a = [[1, 2, 3], "x"]
  a[0][-10..] = [9]
rescue RangeError => e
  puts "#{e.class}: #{e.message}"
end

# promoting a frozen typed array still raises FrozenError
begin
  a = [[1, 2, 3].freeze, "x"]
  a[0][1, 1] = ["s"]
rescue FrozenError => e
  puts e.class
end

# check ORDER matches CRuby: frozen wins over negative length...
begin
  a = [1, 2, 3].freeze
  a[1, -1] = [9]
rescue => e
  puts e.class
end

# ...over a too-small index...
begin
  a = [1, 2, 3].freeze
  a[-9, 1] = [9]
rescue => e
  puts e.class
end

# ...and over an out-of-range range (typed receiver)
begin
  a = [1, 2, 3].freeze
  a[-9..0] = [1]
rescue => e
  puts e.class
end

# the same order through the poly runtime path
begin
  a = [[1, 2, 3].freeze, "x"]
  a[0][1, -1] = [9]
rescue => e
  puts e.class
end
begin
  a = [[1, 2, 3].freeze, "x"]
  a[0][-10..1] = [9]
rescue => e
  puts e.class
end

# negative length is reported before a too-small index (CRuby checks length first)
begin
  a = [1, 2, 3]
  a[-9, -1] = [5]
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end
begin
  y = [1, "x"]
  y[-9, -1] = [5]
rescue IndexError => e
  puts "#{e.class}: #{e.message}"
end

# a range beginning before -len raises RangeError on a typed receiver too
begin
  a = [1, 2, 3]
  a[-9..0] = [1]
rescue RangeError => e
  puts "#{e.class}: #{e.message}"
end

# typed receivers handle beginless / endless range splices
b = [1, 2, 3]
b[..1] = [9]
p b
b = [1, 2, 3]
b[1..] = [8]
p b
