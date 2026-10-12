import os, pathlib, subprocess, sys, time
w = pathlib.Path(sys.argv[1]); state = w/'state'; fs = w/'fs'; fs.mkdir()
(fs/'payload').write_text('payload')
def call(verb, *args, wrapped=False, env=None):
    return subprocess.run([str(w/'rm') if wrapped else './gu-'+verb, '--root', str(state), *map(str,args)], env=dict(os.environ, **(env or {})), capture_output=True, timeout=5)
def create(name):
    p=call('create','--rootfs',fs,name,'/payload'); assert p.returncode==0,p.stderr
failures=[]
for mode in ('eio','kill','term','int'):
    (w/'ready').unlink(missing_ok=True)
    name=mode; create(name); inode=(state/name).stat().st_ino
    env=dict(os.environ, REMOVE_FAULT=mode, REMOVE_READY=str(w/'ready'))
    p=subprocess.Popen([str(w/'rm'),'--root',str(state),name],env=env,stderr=subprocess.PIPE)
    if mode!='eio':
        end=time.monotonic()+5
        while not (w/'ready').exists():
            assert p.poll() is None and time.monotonic()<end
            time.sleep(.01)
        p.send_signal({'kill':9,'term':15,'int':2}[mode])
    _,err=p.communicate(timeout=5)
    assert p.returncode!=0
    print(mode, 'exit', p.returncode, err.decode().strip(), flush=True)
    if (state/name).exists():
        for verb in ('rm','inspect'):
            result=call(verb,name); print('stranded',verb,result.returncode,result.stderr.decode().strip(),flush=True)
        result=call('create','--rootfs',fs,name,'/payload'); print('stranded create',result.returncode,result.stderr.decode().strip(),flush=True)
        failures.append(mode); continue
    stage=state/('.pending-'+name)
    assert stage.stat().st_ino==inode and not (stage/'status').exists()
    assert not (stage/'command').exists()
    assert (stage/'rootfs/payload').read_text()=='payload'
    if mode=='kill':
        assert call('rm',name).returncode==0
        assert not stage.exists()
        assert call('rm',name).returncode!=0 # absent both is an error
    create(name)
    assert not stage.exists()
    assert call('inspect',name).returncode==0
    assert call('rm',name).returncode==0
assert not failures, failures
print('interrupted removal and retry PASS')

for op in ('rename','sync-unpublish','closedir','close','unlink-stage','sync-remove'):
    name='fault-'+op; create(name); stage=state/('.pending-'+name)
    p=call('rm',name,wrapped=True,env={'REMOVE_OP':op,'REMOVE_STAGE':str(stage)})
    assert p.returncode!=0,(op,p.stderr)
    assert (state/name).exists()==(op=='rename'),op
    assert stage.exists()==(op not in ('rename','sync-remove')),op
    if op=='rename':
        assert call('inspect',name).returncode==0
    if op!='sync-remove':
        assert call('rm',name).returncode==0
    create(name); assert call('rm',name).returncode==0
    print(op,'nonzero, correct visibility and retry PASS')
# Stale stage collision is cleared only after published runtime/attachment fences.
import fcntl
for fence in ('runtime','attachment'):
    name='fence-'+fence; create(name); stage=state/('.pending-'+name)
    stage.mkdir(mode=0o700); (stage/'sentinel').write_text('untouched')
    with open(state/name/fence,'w') as held:
        fcntl.lockf(held,fcntl.LOCK_EX)
        p=call('rm',name); assert p.returncode!=0
        assert (state/name/'status').exists() and (stage/'sentinel').exists()
    assert call('rm',name).returncode==0 and not stage.exists()
# Both retry commands reject a live stage even with no status metadata.
for fence in ('runtime','attachment'):
    name='stage-'+fence; stage=state/('.pending-'+name); stage.mkdir(mode=0o700)
    (stage/'sentinel').write_text('untouched')
    with open(stage/fence,'w') as held:
        fcntl.lockf(held,fcntl.LOCK_EX)
        assert call('rm',name).returncode!=0
        assert call('create','--rootfs',fs,name,'/payload').returncode!=0
        assert (stage/'sentinel').exists() and not (state/name).exists()
    assert call('rm',name).returncode==0
for kind in ('symlink','file','public'):
    name='collision-'+kind; create(name); stage=state/('.pending-'+name)
    target=w/('outside-'+kind); target.mkdir(); (target/'sentinel').write_text('safe')
    if kind=='symlink': stage.symlink_to(target)
    elif kind=='file': stage.write_text('safe')
    else: stage.mkdir(mode=0o755); stage.chmod(0o755)
    assert call('rm',name).returncode!=0
    assert (state/name/'status').exists() and (target/'sentinel').read_text()=='safe'
    if kind=='public': stage.rmdir()
    else: stage.unlink()
    assert call('rm',name).returncode==0
# Abandoned creation (no status) is reclaimable through rm, too.
stage=state/'.pending-abandoned'; stage.mkdir(mode=0o700); (stage/'partial').write_text('partial')
assert call('rm','abandoned').returncode==0 and not stage.exists()
print('removal error ordering, collisions, original ownership fences PASS')
