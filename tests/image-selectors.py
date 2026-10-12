#!/usr/bin/env python3
"""Fail if any no-argument image behavior is absent from the common gate."""
import ast, pathlib, re, subprocess
base=pathlib.Path(__file__).resolve().parent.parent
tree=ast.parse((base/'tests/images.py').read_text())
behaviors={node.name for node in tree.body if isinstance(node,ast.FunctionDef) and not node.args.args}
# This fixture takes an injected copy binary supplied by its separate shell gate.
behaviors.remove('race')
assert 'tests/images.py race ' in (base/'tests/image-race.sh').read_text()
script=(base/'tests/images.sh').read_text()
if 'for behavior in $(python3 tests/images.py --list); do' in script:
    selected=subprocess.check_output(['python3',str(base/'tests/images.py'),'--list'],text=True).split()
else:
    match=re.search(r'for behavior in ([^;]+); do',script)
    assert match,'image behavior selector missing'
    selected=match.group(1).split()
assert len(selected)==len(set(selected)),'duplicate image behavior selectors'
assert set(selected)==behaviors,('orphaned',sorted(behaviors-set(selected)),'unknown',sorted(set(selected)-behaviors))
print(f'image selector completeness PASS ({len(selected)} common cases; race has a separate gate)')
