# A method returning `nil | Time` types poly (Time has no first-class
# nullable slot). A caller branch guarded by `a.nil?` or `a` truthiness
# narrows reads of the local to Time inside the non-nil arm (read-site
# unbox, the #1661 machinery on the caller side). Reassigned locals and
# reads outside a guard stay poly and keep the honest NoMethodError.
module M
  def self.maybe_time(s)
    return nil if s.nil?
    Time.at(0).utc
  end
end

# if a.nil? ... else (the issue's shape)
a = M.maybe_time("x")
if a.nil?
  puts "branch-nil"
else
  puts "branch-time"
  y = a.year
  puts "year:" + y.to_s
end

# unless a.nil? (the silent variant from the report)
b = M.maybe_time("x")
unless b.nil?
  puts b.year.to_s
end

# bare truthiness guard, method with an argument
c = M.maybe_time("x")
if c
  puts c.strftime("%Y-%m-%d")
end

# the nil path through the same guards
d = M.maybe_time(nil)
if d.nil?
  puts "d-nil"
else
  puts d.year
end
unless d.nil?
  puts "not-reached"
end

# chained use inside the arm
e = M.maybe_time("x")
unless e.nil?
  puts e.month + e.day
end
puts "end"
