import os
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
t = int((p / "clock").read_text())
os.utime(p / "Makefile", ns=(t, t))
