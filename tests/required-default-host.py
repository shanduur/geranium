#!/usr/bin/env python3
"""A root or explicitly required port-443 gate must fail if binding is denied."""
import contextlib, io, os, sys
from unittest import mock
import images
for uid,required in ((0,'0'),(1000,'1')):
    with mock.patch.object(images.os,'geteuid',return_value=uid), mock.patch.dict(os.environ,GERANIUM_REQUIRE_DEFAULT_HOST=required), mock.patch.object(images.Registry,'__enter__',side_effect=PermissionError('fixture bind denied')), mock.patch.object(images.subprocess,'run'), contextlib.redirect_stdout(io.StringIO()):
        try:
            images.default_host()
        except PermissionError:
            continue
    sys.exit(f'FAIL: required default_host silently skipped (uid={uid}, required={required})')
print('root and explicit required default_host bind failures cannot skip PASS')
