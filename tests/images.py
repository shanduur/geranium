#!/usr/bin/env python3
"""Real-binary registry and image behavior gates; runtime has no Python dependency."""
import ssl, time, os, pathlib, subprocess, tempfile, sys, io, tarfile, gzip, json, hashlib, threading, http.server, contextlib, platform, shlex, stat
BASE = pathlib.Path(__file__).resolve().parent.parent

def published_roots(root):
    return [p for p in pathlib.Path(root).glob('.images/*/rootfs') if not p.parent.name.startswith('.')]

def call(verb, *args, ok=True, env=None):
    p = subprocess.run([str(BASE / ('gu-'+verb)), *map(str,args)], capture_output=True, env=env)
    if (p.returncode == 0) != ok:
        raise AssertionError(f'{verb} {args}: status={p.returncode}\n{p.stdout.decode()}\n{p.stderr.decode()}')
    return p

def cli():
    for verb in ('pull','rmi'):
        assert b'REFERENCE' in call(verb, '--help').stdout
    with tempfile.TemporaryDirectory() as w:
        for ref in ('../escape', 'http://example.com/repo', 'x@sha256:bad', 'a//b', 'host/a:bad tag', 'registry.example:0/repo', 'repo:-tag', 'repo:tag/extra', 'repo@sha256:'+'0'*64+':tag', 'registry.example/-repo'):
            p=call('pull','--root',w,ref,ok=False)
            assert b'invalid image reference' in p.stderr, p.stderr
        p=call('rmi','--root',w,'busybox:latest',ok=False)
        assert b'image not found' in p.stderr, p.stderr
    print('image CLI PASS')

OCI='application/vnd.oci.image.'
def digest(data): return 'sha256:'+hashlib.sha256(data).hexdigest()
def encode(o): return json.dumps(o,separators=(',',':')).encode()
def descriptor(data,media): return dict(mediaType=media,digest=digest(data),size=len(data))
def tar(entries):
    b=io.BytesIO()
    with tarfile.open(fileobj=b,mode='w',format=tarfile.USTAR_FORMAT) as t:
        for entry in entries:
            name,value=entry[:2]
            i=tarfile.TarInfo(name); i.uid=i.gid=0; i.mode=entry[2] if len(entry)>2 else (0o755 if value is None or name.endswith('/busybox') else 0o644)
            if value is None: i.type=tarfile.DIRTYPE; t.addfile(i)
            elif isinstance(value,tuple): i.type=tarfile.LNKTYPE if len(value)==2 else tarfile.SYMTYPE; i.linkname=value[-1]; t.addfile(i)
            else: i.size=len(value); t.addfile(i,io.BytesIO(value))
    return b.getvalue()
class Registry:
    def __init__(self, layers=None, config=None, port=0):
        self.port=port;self.signed_host: str | None = None
        layers=layers or [tar([('etc',None),('etc/marker',b'from registry\n')])]
        self.config=dict(architecture={'x86_64':'amd64','aarch64':'arm64'}.get(platform.machine(),platform.machine()),os='linux',rootfs=dict(type='layers',diff_ids=list(map(digest,layers))),config=config or dict(Cmd=['/bin/sh'],Env=['PATH=/bin:/usr/bin'],WorkingDir='/'))
        c=encode(self.config); self.blobs={digest(c):c}; ds=[]
        for layer in layers:
            b=gzip.compress(layer); self.blobs[digest(b)]=b; ds.append(descriptor(b,OCI+'layer.v1.tar+gzip'))
        self.manifest=encode(dict(schemaVersion=2,mediaType=OCI+'manifest.v1+json',config=descriptor(c,OCI+'config.v1+json'),layers=ds))
        self.requests=[]; self.headers=[];self.responses={};self.auth=False;self.tls: tuple[str | pathlib.Path,str | pathlib.Path] | None = None;self.chunked=False;self.truncate=False;self.redirect: str | None = None
        self.raw_headers: bytes | None = None;self.body_delay=False;self.body_stall=False
        self.chunk_suffix=b'';self.zero_suffix=b''
    def route(self,path,headers):
        if self.signed_host is not None and headers.get('Host')!=self.signed_host:
            return 403,{},b'signed Host mismatch'
        if path.startswith('/token?'): return 200,{},encode(dict(token='fixture-token'))
        if self.auth and headers.get('Authorization')!='Bearer fixture-token':
            scheme='https' if self.tls else 'http'
            return 401,{'WWW-Authenticate':f'Bearer realm="{scheme}://127.0.0.1:{self.server.server_port}/token",service="fixture",scope="repository:fixture:pull"'},b''
        if self.redirect and '/blobs/' in path: return 307,{'Location':self.redirect+path},b''
        if path in self.responses: return self.responses[path]
        if '/manifests/' in path:
            body=self.manifest
            return 200,{'Content-Type':OCI+'manifest.v1+json'},body
        body=self.blobs.get(path.rsplit('/',1)[-1])
        return (200,{},body) if body is not None else (404,{},b'')
    def __enter__(self):
        fixture=self
        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version='HTTP/1.1'
            def log_message(self,*a): pass
            def do_GET(self):
                fixture.requests.append(self.path);fixture.headers.append(dict(self.headers))
                status,headers,body=fixture.route(self.path,self.headers)
                if fixture.raw_headers is not None and '/manifests/' in self.path:
                    self.connection.sendall(fixture.raw_headers + body)
                    return
                self.send_response(status)
                for k,v in headers.items(): self.send_header(k,v)
                self.send_header('Transfer-Encoding' if fixture.chunked else 'Content-Length','chunked' if fixture.chunked else str(len(body)))
                self.send_header('Connection','close');self.end_headers()
                try:
                    if fixture.body_stall and '/manifests/' in self.path:
                        time.sleep(32)
                        self.wfile.write(body)
                    elif fixture.body_delay and '/manifests/' in self.path:
                        step=(len(body)+11)//12
                        for i in range(0,len(body),step):
                            if i: time.sleep(3)
                            self.wfile.write(body[i:i+step]);self.wfile.flush()
                    elif fixture.chunked:
                        for i in range(0,len(body),31):
                            chunk=body[i:i+31];self.wfile.write(f'{len(chunk):x}'.encode()+fixture.chunk_suffix+b'\r\n'+chunk+b'\r\n')
                        self.wfile.write(b'0'+fixture.zero_suffix+b'\r\n\r\n')
                    else: self.wfile.write(body[:-1] if fixture.truncate and body else body)
                except (BrokenPipeError,ConnectionResetError): pass
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',self.port),Handler)
        if self.tls:
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(*self.tls)
            self.server.socket=context.wrap_socket(self.server.socket,server_side=True)
        self.thread=threading.Thread(target=self.server.serve_forever); self.thread.start()
        self.ref=f'127.0.0.1:{self.server.server_port}/fixture:latest'
        return self
    def __exit__(self,*a): self.server.shutdown(); self.server.server_close(); self.thread.join()
def pull():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        roots=list(pathlib.Path(w).glob('.images/*/rootfs'))
        assert len(roots)==1 and (roots[0]/'etc/marker').read_bytes()==b'from registry\n'
        assert len(r.requests)==3, r.requests
        call('pull','--root',w,'--http-loopback',r.ref)
        roots_after=list(pathlib.Path(w).glob('.images/*/rootfs'))
        assert roots_after==roots and (roots_after[0]/'etc/marker').read_bytes()==b'from registry\n'
        assert len(r.requests)==3, r.requests
    print('controlled real registry pull and network-free cache hit PASS')

def remove():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        store=pathlib.Path(w)/'.images'; image=next(p for p in store.iterdir() if not p.name.startswith('.'))
        key=image.name; anchor=store/('.lock-'+key); inode=anchor.stat().st_ino
        # Cleanup must unlink image-controlled symlinks without following them.
        outside=pathlib.Path(w)/'outside';outside.mkdir();(outside/'sentinel').write_text('safe')
        (image/'rootfs'/'escape').symlink_to(outside)
        call('rmi','--root',w,r.ref)
        assert not image.exists() and (outside/'sentinel').read_text()=='safe'
        assert anchor.stat().st_ino==inode
        call('rmi','--root',w,r.ref,ok=False)
        call('pull','--root',w,'--http-loopback',r.ref)
        assert anchor.stat().st_ino==inode
        # Recover an interrupted unpublished removal.
        image.rename(store/('.pending-'+key))
        call('rmi','--root',w,r.ref)
        assert not (store/('.pending-'+key)).exists()
    print('image removal, stable lock and recovery PASS')

def create():
    with tempfile.TemporaryDirectory() as w, Registry(config=dict(Entrypoint=['sh'],Cmd=['-c','echo "$HELLO"'],Env=['PATH=/bin','HELLO=image'],WorkingDir='/work')) as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        call('create','--root',w,'--image',r.ref,'from-image')
        container=pathlib.Path(w)/'from-image'
        assert (container/'rootfs/etc/marker').read_bytes()==b'from registry\n'
        assert (container/'command').read_bytes()==b'sh\0-c\0echo "$HELLO"\0'
        call('rmi','--root',w,r.ref)
        assert (container/'rootfs/etc/marker').exists()
        call('inspect','--root',w,'from-image')
        call('rm','--root',w,'from-image')
    print('image create with defaults and independent copy PASS')

def lifecycle():
    bb=pathlib.Path(os.environ.get('GERANIUM_TEST_BUSYBOX','/usr/bin/busybox')).read_bytes()
    layers=[tar([('bin',None),('bin/busybox',bb),('bin/sh',('busybox',)),('bin/cat',('busybox',)),('work',None)])]
    command='echo "$HELLO:$PATH"; pwd; echo persist >> /work/data; cat /work/data'
    with tempfile.TemporaryDirectory() as w, Registry(layers,dict(Entrypoint=['sh','-c'],Cmd=[command],Env=['PATH=/bin','HELLO=image'],WorkingDir='/work')) as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        call('create','--root',w,'--image',r.ref,'demo')
        p=call('start','--root',w,'demo')
        assert p.stdout==b'image:/bin\n/work\npersist\n',p.stdout
        call('rmi','--root',w,r.ref)
        p=call('start','--root',w,'demo')
        assert p.stdout==b'image:/bin\n/work\npersist\npersist\n',p.stdout
        assert call('logs','--root',w,'demo').stdout==p.stdout
        call('rm','--root',w,'demo')
        call('pull','--root',w,'--http-loopback',r.ref)
        p=call('run','--root',w,'--image',r.ref,'override','/bin/busybox','echo','override')
        assert p.stdout==b'override\n'
        call('rm','--root',w,'override')
    print('image isolated start, environment, cwd, restart, override, run, logs, rmi and rm PASS')

def hardlinks():
    with tempfile.TemporaryDirectory() as w, Registry([tar([('bin',None),('bin/original',b'linked'),('bin/alias',('hardlink','bin/original'))])]) as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        root=next(pathlib.Path(w).glob('.images/*/rootfs'))
        assert (root/'bin/original').stat().st_ino==(root/'bin/alias').stat().st_ino
        call('create','--root',w,'--image',r.ref,'linked','/bin/original')
        root=pathlib.Path(w)/'linked/rootfs'
        assert (root/'bin/original').stat().st_ino==(root/'bin/alias').stat().st_ino
        call('rmi','--root',w,r.ref)
        assert (root/'bin/alias').read_bytes()==b'linked'
        call('rm','--root',w,'linked')
    print('safe image hardlinks preserved through independent copy PASS')

def transport():
    with tempfile.TemporaryDirectory() as w:
        cert=pathlib.Path(w)/'cert.pem';key=pathlib.Path(w)/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=fixture','-addext','subjectAltName=IP:127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
        env=dict(os.environ,SSL_CERT_FILE=str(cert))
        r=Registry();r.tls=(cert,key);r.auth=True;r.chunked=True
        manifest=r.manifest
        index=encode(dict(schemaVersion=2,mediaType=OCI+'index.v1+json',manifests=[dict(descriptor(manifest,OCI+'manifest.v1+json'),platform=dict(os='linux',architecture='wrong')),dict(descriptor(manifest,OCI+'manifest.v1+json'),platform=dict(os='linux',architecture=r.config['architecture']))]))
        r.responses['/v2/fixture/manifests/latest']=(200,{'Content-Type':OCI+'index.v1+json'},index)
        with r:
            call('pull','--root',w+'/untrusted',r.ref,ok=False)
            assert len(r.requests)==0
            hostname_ref=r.ref.replace('127.0.0.1','localhost')
            p=call('pull','--root',w+'/wrong-host',hostname_ref,ok=False,env=env)
            assert b'certificate verification' in p.stderr,p.stderr
            call('pull','--root',w+'/trusted',r.ref,env=env)
            assert any(x.startswith('/token?') for x in r.requests)
            pin=r.ref.split(':latest')[0]+'@'+digest(index)
            r.responses['/v2/fixture/manifests/'+digest(index)]=(200,{'Content-Type':OCI+'index.v1+json'},index)
            call('pull','--root',w+'/pin',pin,env=env)
            wrong=r.ref.split(':latest')[0]+'@sha256:'+'0'*64
            call('pull','--root',w+'/wrong-pin',wrong,ok=False,env=env)
            assert not [p for p in pathlib.Path(w+'/wrong-pin/.images').iterdir() if not p.name.startswith('.')]
        # A redirected origin must never receive the original Bearer token.
        target=Registry();target.tls=(cert,key)
        with target:
            r=Registry();r.tls=(cert,key);r.auth=True;r.redirect=f'https://127.0.0.1:{target.server.server_port}'
            target.blobs=r.blobs
            with r: call('pull','--root',w+'/redirect',r.ref,env=env)
            assert target.headers and all('Authorization' not in h for h in target.headers),target.headers
        r=Registry();r.truncate=True
        with r:
            p=call('pull','--root',w+'/truncated','--http-loopback',r.ref,ok=False)
            assert b'truncated HTTP body' in p.stderr,p.stderr
    print('verified TLS, anonymous Bearer, index/platform, chunking, pinned digest, truncation and cross-origin auth isolation PASS')

def layers():
    base=tar([('.',None,0o750),('dir',None),('dir/old',b'old'),('dir/delete',b'delete'),('retained',b'yes'),('readonly',None,0o555),('readonly/old',b'old')])
    # Opaque marker comes AFTER this layer's additions; additions must survive.
    upper=tar([('dir/new',b'new'),('dir/.wh..wh..opq',b''),('.wh.retained',b''),('tmp',None,0o1777),('readonly/new',b'new')])
    with tempfile.TemporaryDirectory() as w, Registry([base,upper]) as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        root=next(pathlib.Path(w).glob('.images/*/rootfs'))
        assert (root/'dir/new').read_bytes()==b'new'
        assert (root/'dir').stat().st_mode&0o7777==0o755
        assert root.stat().st_mode&0o7777==0o750
        assert not (root/'dir/old').exists() and not (root/'dir/delete').exists() and not (root/'retained').exists()
        assert (root/'tmp').stat().st_mode&0o7777==0o1777
        assert (root/'readonly/new').read_bytes()==b'new'
        assert (root/'readonly').stat().st_mode&0o7777==0o555
        call('create','--root',w,'--image',r.ref,'sticky','/bin/true')
        assert (pathlib.Path(w)/'sticky/rootfs/tmp').stat().st_mode&0o7777==0o1777
        assert (pathlib.Path(w)/'sticky/rootfs').stat().st_mode&0o7777==0o750
        call('rm','--root',w,'sticky')
        # Reclaim an abandoned failed stage on the next successful pull.
        image=root.parent;key=image.name;call('rmi','--root',w,r.ref)
        stage=pathlib.Path(w)/'.images'/('.pending-'+key);stage.mkdir(mode=0o700)
        (stage/'abandoned').write_text('partial')
        call('pull','--root',w,'--http-loopback',r.ref)
        assert not stage.exists() and (image/'rootfs/dir/new').exists()
    print('ordered layers, whiteouts, order-independent opaque marker, sticky dirs and acquisition recovery PASS')
def lower_directories():
    if os.geteuid()==0:
        print('SKIP rootless lower-directory permission regression: requires a non-root caller; musl runner gate runs it')
        return
    failures=[]
    print('rootless lower-directory caller uid/gid',os.geteuid(),os.getegid(),flush=True)
    # Assertion-based version of the independent focused.py reproduction.
    for mode in (0o000,0o100,0o300,0o555):
        with tempfile.TemporaryDirectory() as w:
            try:
                with Registry([tar([('dir',None,mode)]),tar([('dir',None,0o755),('dir/file',b'ok')])]) as r:
                    p=subprocess.run([str(BASE/'gu-pull'),'--root',w,'--http-loopback',r.ref],capture_output=True,timeout=10)
                roots=published_roots(w)
                good=p.returncode==0 and len(roots)==1
                if good:
                    good=(roots[0]/'dir').stat().st_mode&0o7777==0o755 and (roots[0]/'dir/file').read_bytes()==b'ok'
                print('lower-directory-mode',oct(mode),'pull exit',p.returncode,'PASS' if good else 'FAIL','stderr',p.stderr.decode().strip(),flush=True)
                if not good: failures.append((oct(mode),p.returncode,p.stderr,len(roots)))
            finally:
                # A RED pull leaves a mode-000 private stage. Widen only this
                # fixture's known directory so cleanup cannot mask the assertion.
                for directory in pathlib.Path(w).glob('.images/*/rootfs/dir'):
                    if stat.S_ISDIR(directory.lstat().st_mode): os.chmod(directory,0o700,follow_symlinks=False)
    with tempfile.TemporaryDirectory() as w:
        outside=pathlib.Path(w)/'outside';outside.mkdir();(outside/'sentinel').write_bytes(b'safe')
        with Registry([tar([('dir',(str(outside),))]),tar([('dir/file',b'escape')])]) as r:
            p=call('pull','--root',pathlib.Path(w)/'state','--http-loopback',r.ref,ok=False)
        assert b'symlinks forbidden' in p.stderr,p.stderr
        assert not published_roots(pathlib.Path(w)/'state')
        assert (outside/'sentinel').read_bytes()==b'safe' and not (outside/'file').exists()
    assert not failures,failures
    print('rootless lower 000/0100/0300/0555 to upper 0755, publication, contents and symlink-parent rejection PASS')

def reject_layers():
    bad=[tar([('/',None)]),tar([('duplicate',b'one'),('duplicate',b'two')]),tar([('../escape',b'bad')]),tar([('/absolute',b'bad')]),tar([('dir',('/tmp',)),('dir/escape',b'bad')]),tar([('alias',('hardlink','../escape'))]),tar([('setid',b'bad',0o4755)])]
    for layer in bad:
        with tempfile.TemporaryDirectory() as w, Registry([layer]) as r:
            p=call('pull','--root',w,'--http-loopback',r.ref,ok=False)
            assert not [p for p in (pathlib.Path(w)/'.images').iterdir() if not p.name.startswith('.')]
            print('unsafe layer rejected:',p.stderr.decode().strip())
    # Tar extensions and multi-user IDs must be rejected, never ignored.
    for kind in ('pax','owner','device'):
        out=io.BytesIO()
        with tarfile.open(fileobj=out,mode='w',format=tarfile.PAX_FORMAT if kind=='pax' else tarfile.USTAR_FORMAT) as t:
            ti=tarfile.TarInfo('long/'+'a'*150 if kind=='pax' else 'entry')
            if kind=='owner': ti.uid=1
            if kind=='device': ti.type=tarfile.CHRTYPE
            t.addfile(ti)
        with tempfile.TemporaryDirectory() as w, Registry([out.getvalue()]) as r:
            call('pull','--root',w,'--http-loopback',r.ref,ok=False)
    print('traversal, symlink-parent escape, hardlink escape, set-ID, PAX, devices and multi-user ownership rejection PASS')
def integrity():
    for kind in ('http-media','manifest-digest','config-digest','layer-digest','size','diffid','gzip','media','platform','user','json'):
        with tempfile.TemporaryDirectory() as w:
            r=Registry();m=json.loads(r.manifest);c=json.loads(next(iter(r.blobs.values())))
            if kind=='http-media': r.responses['/v2/fixture/manifests/latest']=(200,{'Content-Type':'text/plain'},r.manifest)
            elif kind=='manifest-digest': r.responses['/v2/fixture/manifests/latest']=(200,{'Docker-Content-Digest':'sha256:'+'0'*64},r.manifest)
            elif kind=='config-digest': r.blobs[m['config']['digest']]+=b' '
            elif kind=='layer-digest': r.blobs[m['layers'][0]['digest']]+=b'bad'
            elif kind=='size': m['config']['size']+=1
            elif kind=='media': m['layers'][0]['mediaType']=OCI+'layer.v1.tar+zstd'
            elif kind=='gzip':
                d=b'not gzip';r.blobs[digest(d)]=d;m['layers'][0]=descriptor(d,OCI+'layer.v1.tar+gzip')
            elif kind=='json':
                r.manifest=b'{"schemaVersion":2,"schemaVersion":2}';r.responses['/v2/fixture/manifests/latest']=(200,{},r.manifest)
            else:
                if kind=='diffid': c['rootfs']['diff_ids'][0]='sha256:'+'0'*64
                elif kind=='platform': c['os']='darwin'
                elif kind=='user': c['config']['User']='1000'
                d=encode(c);r.blobs[digest(d)]=d;m['config']=descriptor(d,OCI+'config.v1+json')
            if kind not in ('manifest-digest','json'): r.manifest=encode(m)
            with r: p=call('pull','--root',w,'--http-loopback',r.ref,ok=False)
            assert not [p for p in (pathlib.Path(w)/'.images').iterdir() if not p.name.startswith('.')]
            print(kind,'rejected:',p.stderr.decode().strip())
    print('descriptor sizes/digests, DiffID, gzip, mediaType, platform, User and duplicate JSON integrity PASS')

def race():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        image=next(pathlib.Path(w).glob('.images/*/rootfs')).parent
        ready=pathlib.Path(w)/'ready';release=pathlib.Path(w)/'release'
        env=dict(os.environ,IMAGE_COPY_SOURCE=str(image/'rootfs/etc/marker'),IMAGE_COPY_READY=str(ready),IMAGE_COPY_RELEASE=str(release))
        creator=subprocess.Popen([sys.argv[2],'--root',w,'--image',r.ref,'copy','/bin/true'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        remover=None
        try:
            deadline=time.monotonic()+5
            while not ready.exists():
                assert creator.poll() is None
                assert time.monotonic()<deadline
                time.sleep(.01)
            remover=subprocess.Popen([str(BASE/'gu-rmi'),'--root',w,r.ref],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            time.sleep(.1)
            assert remover.poll() is None,'rmi raced image copy'
            # Image-copy/network work must not serialize unrelated containers.
            unrelated=pathlib.Path(w)/'fixture';unrelated.mkdir()
            call('create','--root',w,'--rootfs',unrelated,'other','/bin/true')
            call('inspect','--root',w,'other');call('rm','--root',w,'other')
            release.touch();out,err=creator.communicate(timeout=5);assert creator.returncode==0,(out,err)
            out,err=remover.communicate(timeout=5);assert remover.returncode==0,(out,err)
            assert (pathlib.Path(w)/'copy/rootfs/etc/marker').read_bytes()==b'from registry\n'
            call('rm','--root',w,'copy')
        finally:
            release.touch()
            for child in (creator,remover):
                if child and child.poll() is None: child.kill();child.wait()
    print('image copy/rmi fence and unrelated container progress PASS')

def partial():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        root=next(pathlib.Path(w).glob('.images/*/rootfs'));image=root.parent
        (image/'config.json').write_bytes(b'')
        call('pull','--root',w,'--http-loopback',r.ref,ok=False)
        call('rmi','--root',w,r.ref)
        call('pull','--root',w,'--http-loopback',r.ref)
        (image/'rootfs').rename(image/'missing-root')
        call('pull','--root',w,'--http-loopback',r.ref,ok=False)
        call('rmi','--root',w,r.ref)
    print('partial or damaged published image never accepted as a usable cache hit PASS')

def path_search():
    bb=pathlib.Path(os.environ.get('GERANIUM_TEST_BUSYBOX','/usr/bin/busybox')).read_bytes()
    for search in ('bin',':/missing:bin:', '/missing:bin'):
        with tempfile.TemporaryDirectory() as w, Registry([tar([('bin',None),('bin/busybox',bb)])],dict(Cmd=['busybox','echo','image-path'],Env=['PATH='+search],WorkingDir='/')) as r:
            call('pull','--root',w,'--http-loopback',r.ref)
            p=call('run','--root',w,'--image',r.ref,'path')
            assert p.stdout==b'image-path\n',p.stdout
            call('rm','--root',w,'path')
    print('image PATH relative and empty component semantics inside rootfs PASS')

def plain_bounds():
    with tempfile.TemporaryDirectory() as w:
        r=Registry();m=json.loads(r.manifest);old=m['layers'][0]['digest'];raw=gzip.decompress(r.blobs[old]);r.blobs[digest(raw)]=raw;m['layers'][0]=descriptor(raw,OCI+'layer.v1.tar');r.manifest=encode(m)
        with r: call('pull','--root',w+'/plain','--http-loopback',r.ref)
        for kind in ('framing','headers','json-size','json-depth','basic','downgrade','realm'):
            r=Registry()
            if kind=='framing': r.responses['/v2/fixture/manifests/latest']=(200,{'Transfer-Encoding':'chunked'},r.manifest)
            elif kind=='headers': r.responses['/v2/fixture/manifests/latest']=(200,{'X-Excessive':'x'*32768},r.manifest)
            elif kind=='json-size': r.manifest=b'x'*(1024*1024+1)
            elif kind=='json-depth': r.manifest=b'['*34+b'0'+b']'*34
            elif kind=='basic': r.responses['/v2/fixture/manifests/latest']=(401,{'WWW-Authenticate':'Basic realm="private"'},b'')
            elif kind=='realm': r.responses['/v2/fixture/manifests/latest']=(401,{'WWW-Authenticate':'Bearer realm="http://example.com/token",service="x",scope="x"'},b'')
            elif kind=='downgrade': r.responses['/v2/fixture/manifests/latest']=(307,{'Location':'http://example.com/elsewhere'},b'')
            with r: p=call('pull','--root',w+'/'+kind,'--http-loopback',r.ref,ok=False)
            print(kind,'rejected:',p.stderr.decode().strip())
        # File-size and depth bounds are tested before filesystem publication.
        for kind in ('file-size','depth'):
            out=io.BytesIO()
            with tarfile.open(fileobj=out,mode='w',format=tarfile.USTAR_FORMAT) as t:
                entry=tarfile.TarInfo('large' if kind=='file-size' else '/'.join(['a']*33))
                entry.size=256*1024*1024+1 if kind=='file-size' else 0
                t.addfile(entry)
            with Registry([out.getvalue()]) as r: p=call('pull','--root',w+'/'+kind,'--http-loopback',r.ref,ok=False)
            print(kind,'rejected:',p.stderr.decode().strip())
    print('plain tar, framing/header/JSON/file/depth bounds, private auth and unsafe HTTP URL rejection PASS')

def opaque():
    layers=[tar([('base',b'base')]),
            tar([('.wh..wh..opq',b''),('middle',b'middle')]),
            tar([('.wh..wh..opq',b''),('final',b'final')])]
    with tempfile.TemporaryDirectory() as w, Registry(layers) as r:
        call('pull','--root',w,'--http-loopback',r.ref)
        root=next(pathlib.Path(w).glob('.images/*/rootfs'))
        assert sorted(p.name for p in root.iterdir())==['final'], list(root.iterdir())
        assert (root/'final').read_bytes()==b'final'
    print('repeated root opaque whiteouts remove lower layers and retain same-layer additions PASS')

def raw_transport():
    for kind in ('valid','nul','bare-lf','bare-cr','empty-length','empty-encoding','invalid-name','invalid-version'):
        with tempfile.TemporaryDirectory() as w, Registry() as r:
            status=b'HTTP/1.9 200 OK\r\n' if kind=='invalid-version' else b'HTTP/1.1 200 OK\r\n'
            fields=f'Content-Type: {OCI}manifest.v1+json\r\n'.encode()
            if kind=='empty-length': fields+=b'Content-Length:\r\n'
            fields+=f'Content-Length: {len(r.manifest)}\r\n'.encode()
            if kind=='nul': fields+=b'X-Unknown: ignored\x00\r\nTransfer-Encoding: chunked\r\n'
            elif kind=='bare-lf': fields+=b'X-Unknown: ignored\nY-Unknown: ignored\r\n'
            elif kind=='bare-cr': fields+=b'X-Unknown: ignored\rY-Unknown: ignored\r\n'
            elif kind=='empty-encoding': fields+=b'Transfer-Encoding:\r\n'
            elif kind=='invalid-name': fields+=b'Bad Header: ignored\r\n'
            elif kind=='valid': fields+=b'X-Unknown:\r\n'
            r.raw_headers=status+fields+b'\r\n'
            call('pull','--root',w,'--http-loopback',r.ref,ok=kind=='valid')
            assert bool(published_roots(w))==(kind=='valid'),kind
    print('raw HTTP NUL/CRLF/empty framing/field-name/version rejection and valid empty unknown header PASS')

def slow_transport():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        r.body_delay=True
        begin=time.monotonic()
        call('pull','--root',w,'--http-loopback',r.ref)
        assert time.monotonic()-begin>=30
        assert published_roots(w)
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        r.body_stall=True
        p=call('pull','--root',w,'--http-loopback',r.ref,ok=False)
        assert b'timeout' in p.stderr or b'deadline' in p.stderr,p.stderr
        assert not published_roots(w)
    print('progressing HTTP body beyond thirty seconds succeeds; idle body is rejected without publication PASS')

def optional_metadata():
    for indexed in (False,True):
        with tempfile.TemporaryDirectory() as w, Registry() as r:
            manifest=json.loads(r.manifest);manifest.pop('mediaType');child=encode(manifest)
            r.manifest=child
            if indexed:
                selected=dict(descriptor(child,OCI+'manifest.v1+json'),platform=dict(os='linux',architecture=r.config['architecture']))
                index=encode(dict(schemaVersion=2,manifests=[selected]))
                r.responses['/v2/fixture/manifests/latest']=(200,{'Content-Type':OCI+'index.v1+json; charset=utf-8'},index)
                r.responses['/v2/fixture/manifests/'+digest(child)]=(200,{'Content-Type':OCI+'manifest.v1+json; charset=utf-8'},child)
            call('pull','--root',w,'--http-loopback',r.ref)
            assert published_roots(w)
    print('optional OCI index/manifest media type PASS')

def long_metadata():
    with tempfile.TemporaryDirectory() as w, Registry() as r:
        config=json.loads(encode(r.config))
        config['history']=[dict(created_by='x'*9000),dict(created_by='quoted " and slash \\ and emoji 🙂'*500)]
        data=encode(config);r.blobs[digest(data)]=data
        manifest=json.loads(r.manifest);manifest['config']=descriptor(data,OCI+'config.v1+json');r.manifest=encode(manifest)
        call('pull','--root',w,'--http-loopback',r.ref)
        assert published_roots(w)
    with tempfile.TemporaryDirectory() as w, Registry(config=dict(Cmd=['x'*9000])) as r:
        p=call('pull','--root',w,'--http-loopback',r.ref,ok=False)
        assert b'image command too large' in p.stderr,p.stderr
        assert not published_roots(w)
    print('long ignored JSON history and retained command budget PASS')

def authority():
    failures=[]
    with tempfile.TemporaryDirectory() as w:
        w=pathlib.Path(w);helper=w/'authority';key=w/'server.key';cert=w/'server.pem'
        command=[os.environ.get('CC','cc')]
        for flags in ('CPPFLAGS','CFLAGS'): command+=shlex.split(os.environ.get(flags,'-O2 -Wall -Wextra -Werror -pedantic' if flags=='CFLAGS' else ''))
        command+=['-std=c99',str(BASE/'tests/registry-authority.c'),str(BASE/'src/json.c')]
        for flags in ('LDFLAGS','LDLIBS'): command+=shlex.split(os.environ.get(flags,''))
        subprocess.run(command+['-lssl','-lcrypto','-o',str(helper)],check=True)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=fixture','-addext','subjectAltName=IP:127.0.0.1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        env=dict(os.environ,SSL_CERT_FILE=str(cert))
        for scheme,default,other in (('http','80','8080'),('https','443','8443')):
            # Direct requests and relative redirects must preserve every spelling.
            cases=[(port,relative,None) for port in ('',default,other) for relative in (False,True)]
            # An explicit default port and an omitted port share origin identity;
            # a different port strips auth even when the test remaps the socket.
            cases += [('',False,default),(default,False,''),('',False,other)]
            for port,relative,target in cases:
                host='127.0.0.1'+(':'+port if port else '')
                final_host='127.0.0.1'+(':'+target if target else '') if target is not None else host
                r=Registry();r.tls=(cert,key) if scheme=='https' else None
                r.responses['/final']=(200,{},b'ok')
                location='/final' if relative else f'{scheme}://{final_host}/final'
                r.responses['/start']=(307,{'Location':location},b'') if relative or target is not None else (200,{},b'ok')
                with r:
                    p=subprocess.run([str(helper),f'{scheme}://{host}/start',str(r.server.server_port)],env=env,capture_output=True,timeout=15)
                expected=[host,final_host] if relative or target is not None else [host]
                actual=[h.get('Host') for h in r.headers]
                expected_ports=[port or default]+([target or default] if target is not None else [port or default] if relative else [])
                identities=p.stdout.decode().splitlines()
                expected_auth=[True, target!=other] if target is not None else [True]*len(expected)
                actual_auth=[h.get('Authorization')=='Bearer fixture-token' for h in r.headers]
                label=f'{scheme} port={port or "omitted"} relative={relative} target={target!r}'
                good=p.returncode==0 and actual==expected and actual_auth==expected_auth and identities==['connection identity 127.0.0.1:'+v for v in expected_ports]
                print(label, 'PASS' if good else 'FAIL', 'Host=',actual,'auth=',actual_auth,flush=True)
                if not good: failures.append((label,p.returncode,p.stderr,actual,expected,identities))
    assert not failures,failures
    print('production request authority, relative redirects, normalized origins and auth stripping PASS')

def default_host():
    with tempfile.TemporaryDirectory() as w:
        w=pathlib.Path(w);key=w/'server.key';cert=w/'server.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=127.0.0.1','-addext','subjectAltName=IP:127.0.0.1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        env=dict(os.environ,SSL_CERT_FILE=str(cert))
        try:
            with Registry(port=443) as dst:
                # This plain probe only tests permission/availability of the port.
                pass
        except PermissionError:
            if os.geteuid()==0 or os.environ.get('GERANIUM_REQUIRE_DEFAULT_HOST')=='1':
                raise
            print('SKIP default_host wire test: permission to bind TLS port 443 denied (required in root musl gate)')
            return
        with Registry() as src:
            dst=Registry(port=443);dst.tls=(cert,key);dst.blobs=src.blobs.copy();dst.signed_host='127.0.0.1'
            with dst:
                src.redirect='https://127.0.0.1'
                p=subprocess.run([str(BASE/'gu-pull'),'--root',str(w/'state'),'--http-loopback',src.ref],env=env,capture_output=True,timeout=15)
                assert dst.headers,(p.returncode,p.stderr)
                assert all(h.get('Host')=='127.0.0.1' for h in dst.headers),(p.returncode,p.stderr,dst.headers)
                assert p.returncode==0,p.stderr
                assert published_roots(w/'state')
    print('verified TLS redirected blob target receives canonical default-port Host PASS')

def raw_chunks():
    for kind in ('valid','extension','nul-extension','nul-zero-extension','nul-zero-digit'):
        with tempfile.TemporaryDirectory() as w, Registry() as r:
            r.chunked=True
            if kind=='extension': r.chunk_suffix=b';unsupported=value'
            elif kind=='nul-extension': r.chunk_suffix=b'\x00;unsupported=value'
            elif kind=='nul-zero-extension': r.zero_suffix=b'\x00;unsupported=value'
            elif kind=='nul-zero-digit': r.zero_suffix=b'\x00f'
            call('pull','--root',w,'--http-loopback',r.ref,ok=kind=='valid')
            assert bool(published_roots(w))==(kind=='valid'),kind
    print('real chunked framing rejects NUL-hidden ordinary/terminal extensions and digits PASS')

# Single common selector list, consumed by images.sh in native/musl/CI gates.
# image-selectors.py checks every no-argument behavior against this registration.
CASES=('cli','pull','remove','create','hardlinks','transport','plain_bounds',
       'layers','lower_directories','reject_layers','integrity','partial',
       'lifecycle','path_search','opaque','raw_transport','slow_transport',
       'optional_metadata','long_metadata','authority','default_host','raw_chunks')
if __name__=='__main__':
    if sys.argv[1]=='--list':
        print('\n'.join(CASES))
    elif sys.argv[1] in CASES or sys.argv[1]=='race':
        globals()[sys.argv[1]]()
    else:
        sys.exit('unknown image behavior: '+sys.argv[1])
