# Time sub-second precision, fixed utc_offset construction, exact Float
# shifts, and value-based hash.

# usec through Time.utc's 7th argument
t = Time.utc(2007, 11, 1, 15, 25, 0, 123456)
p t.usec
p t.nsec
p t.subsec
p t.inspect
p t.to_s          # to_s never renders the fraction
puts t            # puts uses to_s

# subsec of a whole second is Integer 0
p Time.utc(2020, 1, 1).subsec
p Time.utc(2020, 1, 1).inspect

# fraction rendering trims trailing zeros
p Time.at(0.5).utc.inspect
p Time.at(1.25).utc.inspect

# usec range checks
begin; Time.utc(2000, 1, 1, 0, 0, 0, 1000000); rescue ArgumentError => e; puts "#{e.class}: #{e.message}"; end
begin; Time.utc(2000, 1, 1, 0, 0, 0, -1); rescue ArgumentError => e; puts "#{e.class}: #{e.message}"; end

# Time.local also takes the usec argument (zone-independent accessors only)
l = Time.local(1997, 11, 21, 9, 55, 6, 42)
p l.usec
p l.nsec

# exact Float shifts: the double's binary value decides the nanosecond
p (Time.at(100) + -1.3).usec
p (Time.at(100) - 1.3).usec
p (Time.at(0) + 0.5).nsec
p (Time.at(10) + 2.25).to_f
p Time.at(-0.5).to_i
p Time.at(-0.5).nsec

# shifts preserve the zone kind and the sub-second value
s = Time.utc(2000, 1, 1, 0, 0, 0, 250000)
p (s + 1).usec
p (s + 1).utc?
p (s + 1).inspect

# Time.new with an explicit utc_offset (7th argument)
o = Time.new(2000, 1, 1, 0, 0, 0, 123)
p o.utc_offset
p o.utc?
p o.inspect
p o.hour

n = Time.new(2000, 1, 1, 12, 0, 0, "+09:00")
p n.utc_offset
p n.inspect
p n.hour

m = Time.new(2000, 1, 1, 12, 0, 0, -3600)
p m.utc_offset
p m.inspect

# offset bounds
begin; Time.new(2000, 1, 1, 0, 0, 0, 86400); rescue ArgumentError => e; puts "#{e.class}: #{e.message}"; end

# equal instants hash equal; Time works as a Hash key
a = Time.at(1234)
b = Time.at(1234)
p a.hash == b.hash
p a.hash == Time.at(1235).hash
u = Time.at(1.5)
v = Time.at(1.5)
p u.hash == v.hash
h = { Time.at(99) => "x" }
p h[Time.at(99)]
