# .class on an int/float slot holding the nil sentinel (a nil-returning
# <=>, Complex#infinite? on a finite value) reports NilClass, matching
# how p prints it -- not the slot's static Integer/Float.
z = Complex(2, 3).infinite?
p z
p z.class
