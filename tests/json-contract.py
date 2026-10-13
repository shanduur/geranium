#!/usr/bin/env python3
"""Private syntax/numeric edges that image semantics cannot expose exactly."""
import subprocess, sys
binary=sys.argv[1]
def check(label,body,ok=True,uint=False,output=b''):
    p=subprocess.run([binary]+(['uint'] if uint else []),input=body,capture_output=True)
    assert p.returncode==(0 if ok else 1),(label,p.returncode,p.stderr)
    assert p.stdout==output,(label,p.stdout)
    assert p.stderr==(b'' if ok else b'geranium image: invalid or excessive JSON\n'),(label,p.stderr)
for value in (b'0',b'18446744073709551615'):
    check('exact uint '+value.decode(),value,uint=True,output=value+b'\n')
for value in (b'-0',b'-1',b'1.0',b'1e0',b'18446744073709551616',b'null',b'"1"',b'true',b'[]',b'{}'):
    check('uint type/form '+value.decode(),value,ok=False,uint=True)
for value in (b'-0',b'1.0',b'1e0',b'1e9999',b'1'*64): check('valid raw number',value)
check('number65',b'1'*65,ok=False)
check('root depth32',b'['*32+b'0'+b']'*32)
check('root depth33',b'['*33+b'0'+b']'*33,ok=False)
check('empty container depth32',b'['*33+b']'*33)
check('values32768',b'['+b','.join([b'null']*32767)+b']')
check('values32769',b'['+b','.join([b'null']*32768)+b']',ok=False)
check('keys excluded',b'{'+b','.join(b'"k%d":null'%i for i in range(32767))+b'}')
check('input1MiB',b'0'+b' '*(1024*1024-1))
check('input1MiB+1',b'0'+b' '*(1024*1024),ok=False)
for value in (b'',b' ',b'0x1',b'+1',b'01',b'1.',b'1e',b'NaN',b'Infinity',b'0 0',b'0x',b'{}x',b'[0,]',b'{"x":0,}',b'/*x*/0',b'"\\q"',b'"\\udc00"',b'"\\ud800"',b'"\xed\xa0\x80"',b'"\xf4\x90\x80\x80"',b'"\x80"',b'"\xe2\x82"',b'"\x01"',b'0\0',b'"\0"'):
    check(repr(value),value,ok=False)
print('private JSON syntax, exact uint, input/depth/value/key budgets PASS')
