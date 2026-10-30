import hashlib
import json
import pathlib
import sys

p = next(pathlib.Path(sys.argv[1]).glob(".images/*/manifest.json"))
print(
    "Public Docker Hub selected manifest: sha256:"
    + hashlib.sha256(p.read_bytes()).hexdigest()
)
c = json.loads(p.parent.joinpath("config.json").read_bytes())
print(
    "Public image platform:", c["os"], c["architecture"], "command:", c["config"]["Cmd"]
)
