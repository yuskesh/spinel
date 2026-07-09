# $stdin / STDIN are IO handles over the C stdin stream: gets (nil at EOF),
# read, each_line, eof?, tty? route through the ordinary TY_IO surface.
line = $stdin.gets
p line
p STDIN.gets
p $stdin.gets            # EOF -> nil
p $stdin.eof?
p $stdin.tty?            # piped in the test harness -> false
