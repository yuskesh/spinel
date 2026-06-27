# IO#tty?/#isatty, IO#<< (chainable), and IO#fileno on STDOUT/STDERR. The 12-gem
# "unsupported call: tty?/<</winsize on STDOUT/STDERR" cluster (#1605): STDOUT
# types as a concrete IO, so these IO methods must be modelled rather than
# rejected. The test harness redirects stdout/stderr, so tty? is false.
puts STDOUT.tty?
puts STDERR.isatty
STDOUT << "a" << "b" << "c\n"
puts STDOUT.fileno
puts STDERR.fileno
