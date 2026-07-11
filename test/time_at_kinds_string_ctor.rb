# Time.at with Rational / Time / non-numeric arguments, the 10-argument
# reversed constructor form, and the string-parsing Time.new.

# Time.at(Rational) is the exact fractional epoch, floored to the nanosecond
p Time.at(Rational(1500, 1000)).nsec
p Time.at(Rational(3, 2)).to_f
p Time.at(Rational(-1, 3)).to_i
p Time.at(Rational(-1, 3)).nsec
p Time.at(Rational(7, 1)).nsec

# Time.at(Time) copies the instant
p Time.at(Time.at(55)).to_i
p Time.at(Time.at(1.5)).nsec

# non-numeric arguments raise CRuby's TypeError
def at_it(x); Time.at(x); end
begin; at_it("0"); rescue TypeError => e; puts "#{e.class}: #{e.message}"; end
begin; Time.at(nil); rescue TypeError => e; puts "#{e.class}: #{e.message}"; end
begin; Time.at(:sym); rescue TypeError => e; puts "#{e.class}: #{e.message}"; end

# 10-argument reversed form (sec, min, hour, day, mon, year, wday, yday, isdst, tz)
p Time.utc(1, 15, 20, 1, 1, 2000, 0, 0, 0, 0).inspect
p Time.gm(30, 0, 12, 25, 12, 1999, 0, 0, 0, 0).inspect
p Time.local(1, 15, 20, 1, 1, 2000, 0, 0, 0, 0).year

# string-parsing Time.new
t = Time.new("2021-12-25 10:00:00 +09:00")
p t.month
p t.utc_offset
p t.inspect
p t.hour
u = Time.new("2021-12-25 10:00:00 UTC")
p u.utc?
p u.inspect
n = Time.new("2021-12-25 10:00:00")
p n.hour
p n.min
f = Time.new("2021-12-25 10:00:00.5 +00:00")
p f.nsec
p f.inspect

# unparseable strings raise CRuby's ArgumentError messages
def parse_it(s); Time.new(s); end
begin; parse_it("garbage"); rescue ArgumentError => e; puts "#{e.class}: #{e.message}"; end
begin; parse_it("2021-12-25"); rescue ArgumentError => e; puts "#{e.class}: #{e.message}"; end
