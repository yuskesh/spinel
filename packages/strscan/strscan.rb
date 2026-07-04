# Spinel lib: strscan
#
# StringScanner is a builtin class — methods are intercepted in
# codegen and dispatched to the sp_StringScanner_* helpers in
# lib/sp_strscan.c (linked from libspinel_rt.a). This file is
# intentionally empty so spinel doesn't try to emit a struct
# for the class. `require "strscan"` still has to find the file
# (the parser includes it via resolve_plain_requires).
