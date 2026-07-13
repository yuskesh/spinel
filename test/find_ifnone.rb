# find/detect with an ifnone proc: called only when no element matches;
# a match (including a nil element) ignores it. The result carries whatever
# the proc returns.
def f_int(a) = a.find(-> { -1 }) { |x| x > 10 }
p f_int([1, 2, 3])
p f_int([1, 20, 3])

def f_str(a) = a.detect(-> { "none" }) { |s| s.start_with?("z") }
p f_str(["ab", "cd"])
p f_str(["ab", "zx"])

def f_poly(a) = a.find(-> { :missing }) { |x| x == 42 }
p f_poly([1, "two", :three])
p f_poly([1, 42, "three"])

def f_nil_ret(a) = a.find(-> { nil }) { |x| x > 99 }
p f_nil_ret([1, 2])

def f_nil_elem(a) = a.find(-> { :fallback }) { |x| x.nil? }
p f_nil_elem([1, nil, 3])

ifnone_ran = 0
counter = -> { ifnone_ran += 1; :ran }
p [5, 6].find(counter) { |x| x > 5 }
p ifnone_ran
p [5, 6].find(counter) { |x| x > 9 }
p ifnone_ran

def f_raise(a)
  a.find(-> { raise ArgumentError, "empty search" }) { |x| x > 9 }
rescue => e
  "#{e.class}: #{e.message}"
end
p f_raise([1, 2])
