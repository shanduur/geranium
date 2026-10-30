import os
import pathlib
import sys

p = pathlib.Path(sys.argv[1])
files = [*p.glob("*.o"), *p.glob("libgeranium.*"), *p.glob("gu-*")]
t = max(x.stat().st_mtime_ns for x in files)
(p / "clock").write_text(str(t))
for x in files:
    os.utime(x, ns=(t, t))
