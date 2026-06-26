# arr[i] ||= v across array element kinds (IndexOrWriteNode, statement + expr).
# Each receiver is routed through a per-type helper so its concrete array type
# survives (one shared helper would unify every kind to a boxed poly value).
def si(x); x; end
def sp(x); x; end
def sf(x); x; end
def sg(x); x; end

# int array: a present slot is kept (0 is truthy in Ruby), nil slots written
a = si([1, 2, 3])
a[0] ||= 9          # kept
a[1] ||= 8          # kept
p a

z = si([0, 2, 3])
z[0] ||= 9          # 0 is truthy -> kept
p z

# nilable int (-> poly) array: the nil slot is filled
n = sp([1, nil, 3])
n[1] ||= 8
p n

# poly array: falsy (nil) slot filled, truthy slot kept
q = sp([1, "y", nil])
q[2] ||= 9
q[0] ||= 7
p q

# float array: present slot kept
f = sf([1.0, 2.0])
f[0] ||= 9.0
p f

# string array: present slot kept
g = sg(["x", "y"])
g[0] ||= "z"
p g

# expression position: the form yields the resulting slot value
r = (n[1] ||= 5)    # already 8 -> kept, yields 8
p [r, n]
