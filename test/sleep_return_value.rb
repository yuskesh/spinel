# sleep returns the Integer number of seconds slept (0 for a zero duration),
# not nil. Cover bare, assigned, Kernel-qualified (plain and root-qualified),
# and arithmetic-consumer forms.
p sleep(0)
x = sleep(0)
p x
p Kernel.sleep(0)
p ::Kernel.sleep(0)
n = sleep(0) + 5
p n
p sleep(0.0)
