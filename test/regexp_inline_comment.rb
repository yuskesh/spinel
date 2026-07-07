# (?#comment) groups parse as empty atoms.
p(/foo(?#comment)bar/.match("foobar") ? "m" : "n")
p(/foo(?#)bar/ =~ "xfoobar")
