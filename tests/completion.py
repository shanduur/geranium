import pathlib, subprocess as sp, os, shutil, time, fcntl, struct, signal, sys
w=pathlib.Path(sys.argv[1]); b=pathlib.Path.cwd(); fs=w/'fs'; (fs/'bin').mkdir(parents=True)
shutil.copy(os.environ.get('GERANIUM_TEST_BUSYBOX','/usr/bin/busybox'),fs/'bin/busybox'); state=w/'state'
def call(v,*args,**kw):
    return sp.run([str(b/('gu-'+v)),'--root',str(state),*map(str,args)],capture_output=True,timeout=10,**kw)
def launch(binary,name,script,env):
    return sp.Popen([str(w/binary),'--root',str(state),'--rootfs',str(fs),name,'/bin/busybox','sh','-c',script],env=env,stdout=sp.PIPE,stderr=sp.PIPE)
def waitfor(path):
    end=time.monotonic()+5
    while not path.exists():
        assert time.monotonic()<end, str(path); time.sleep(.005)
def killinit(name):
    with open(state/name/'runtime','r+b') as fd:
        raw=fcntl.fcntl(fd,fcntl.F_GETLK,struct.pack('hhqqi',fcntl.F_WRLCK,os.SEEK_SET,0,0,0))
        typ,_,_,_,pid=struct.unpack('hhqqi',raw); assert typ!=fcntl.F_UNLCK and pid>0
        # Runtime remains leased by the paused init throughout this signal.
        os.kill(pid,signal.SIGKILL)
for code in (0,42):
    r=call('run','--rootfs',fs,'normal'+str(code),'/bin/busybox','sh','-c','exit '+str(code)); assert r.returncode==code,r
    assert (state/('normal'+str(code))/'exitcode').read_text()==str(code)+'\n'
    assert call('rm','normal'+str(code)).returncode==0
# Queue stop (holding the name lock) and inspect while init is paused; also
# test foreground-first and inspect-first completion schedules repeatedly.
for schedule in ('foreground','inspect','stop'):
    for code in (0,42):
        name=schedule+str(code); p=launch('completion',name,'exit '+str(code),dict(os.environ,COMPLETION_FAULT='kill'))
        observer=None
        try:
            waitfor(state/name/'result-window')
            assert (state/name/'exitcode').read_text()==str(code)+'\n'
            assert (state/name/'status').read_text()=='running\n'
            if schedule=='inspect':
                r=call('inspect',name); assert b'status=running' in r.stdout,r
            if schedule=='stop':
                observer=sp.Popen([str(b/'gu-stop'),'--root',str(state),name],stdout=sp.PIPE,stderr=sp.PIPE)
                time.sleep(.1)
            killinit(name)
            if schedule=='inspect': assert call('inspect',name).returncode==0
            out,err=p.communicate(timeout=6); assert p.returncode==125,(schedule,code,p.returncode,out,err)
            if observer:
                oo,ee=observer.communicate(timeout=6); assert observer.returncode==0,(oo,ee)
            assert (state/name/'status').read_text()=='exited\n'
            assert not (state/name/'exitcode').exists()
            assert b'status=exited' in call('inspect',name).stdout
            assert call('rm',name).returncode==0
        finally:
            if p.poll() is None: killinit(name); p.kill(); p.wait()
            if observer and observer.poll() is None: observer.kill();observer.wait()
        print('completion SIGKILL',schedule,code,'PASS',flush=True)
for fault in ('status-sync','directory-sync'):
    p=launch('completion',fault,'echo FINAL; exit 42',dict(os.environ,COMPLETION_FAULT=fault))
    out,err=p.communicate(timeout=8); assert p.returncode==125,(fault,p.returncode,out,err)
    assert b'FINAL\n' in out and b'publish status:' in out,(fault,out,err)
    assert (state/fault/'status').read_text()=='exited\n'
    assert not (state/fault/'exitcode').exists()
    assert call('inspect',fault).returncode==0
    assert call('rm',fault).returncode==0
    print('completion injected',fault,'PASS',flush=True)
for fault in (False,True):
    name='tail'+str(fault); env=dict(os.environ);env.pop('FAIL_TAIL',None)
    if fault:env['FAIL_TAIL']='1'
    p=launch('tail',name,'/bin/busybox sleep .2; echo FINAL-TAIL',env)
    out,err=p.communicate(timeout=8)
    assert p.returncode==(1 if fault else 0),(p.returncode,out,err)
    assert out==(b'' if fault else b'FINAL-TAIL\n'),out
    assert (state/name/'log').read_bytes()==b'FINAL-TAIL\n'
    if fault:assert b'read final log:' in err,err
    assert call('rm',name).returncode==0
    print('final unread tail',fault,'PASS',flush=True)
assert call('create','--rootfs',fs,'output','/bin/busybox','true').returncode==0
assert call('inspect','output').stdout==b'name=output\nstatus=created\n'
readonly=w/'readonly'; readonly.write_bytes(b'unchanged')
for verb in ('create','run','start','stop','inspect','logs','rm','pull','rmi'):
    for args in (['--help'],['--root',str(state),'--help']):
        for dest in (pathlib.Path('/dev/full'),readonly):
            with open(dest,'wb' if str(dest)=='/dev/full' else 'rb') as fd:
                r=sp.run([str(b/('gu-'+verb)),*args],stdout=fd,stderr=sp.PIPE,timeout=5)
                assert r.returncode==1,(verb,args,dest,r)
    print('help output errors',verb,'PASS',flush=True)
for dest in (pathlib.Path('/dev/full'),readonly):
    with open(dest,'wb' if str(dest)=='/dev/full' else 'rb') as fd:
        r=sp.run([str(b/'gu-inspect'),'--root',str(state),'output'],stdout=fd,stderr=sp.PIPE,timeout=5)
        assert r.returncode==1 and b'write inspect:' in r.stderr,r
assert readonly.read_bytes()==b'unchanged'
assert call('rm','output').returncode==0
print('completion/read/output regressions PASS',flush=True)
