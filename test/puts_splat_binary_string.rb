# puts(*a) writes each String element of the splat by its byte length, so
# an embedded NUL is printed (a StrArray element arrives as a String handle).
pieces = Array.new(3, "")
d = "xx" + 0.chr + "AB" + 255.chr + "yy"
pieces[0] = d.byteslice(2, 3)
pieces[1] = d.byteslice(1, 4)
pieces[2] = "C" + 0.chr + "D"
f = pieces.join
puts f.bytesize
puts f.bytes.join(",")
a = ["P" + 0.chr + "Q"]
puts(*a)
puts(*["x" + 0.chr + "y", "z"])
