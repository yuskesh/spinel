# Imported mruby-regexp engine fix.
#
# 581092a75 (\k<name> named backreferences): named groups (?<n>...) existed but
# \k<name> / \k'name' to reference them did not, so /(?<n>\w+)\k<n>/ never
# matched. Numeric \k<1> (absolute) and relative \k<-1> forms also resolve to
# the existing RE_BACKREF opcode.
p(/(?<n>\w+)\k<n>/.match?("abcabc"))           # true
p(/(?<n>\w+)\k<n>/.match?("abcdef"))           # false
p(/(?<n>\w+)\k<n>/.match("abcabc")[0])         # "abcabc"
p(/(?<n>\w+)\k<n>/.match("abcabc")[1])         # "abc"
p(/(?<a>.)(?<b>.)\k<a>\k<b>/.match?("xyxy"))   # true
p(/(?<a>.)(?<b>.)\k<a>\k<b>/.match?("xyyx"))   # false
p(/(\w)\k<1>/.match?("zz"))                    # true (numeric)
p(/(\w)\k<1>/.match?("xy"))                    # false
p(/(.)\k<-1>/.match?("aa"))                    # true (relative)
p(/(?<n>\w+)\k'n'/.match?("abcabc"))           # true (quote form)
