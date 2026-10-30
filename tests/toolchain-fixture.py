import sys

p = sys.argv[1]
s = open(p).read()
lines = s.splitlines()
recipe = "\n".join(
    x
    for x in lines
    if any("sh tests/" + name + ".sh" in x for name in ("readdir-eio", "removal"))
)
open(p, "w").write(s + "\nfixture-test: all\n" + recipe + "\n")
