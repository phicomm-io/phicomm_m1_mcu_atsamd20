#!/usr/bin/env python3
"""Record a locally built candidate; never flashes or publishes an artifact."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def output(*args):
    return subprocess.check_output(args,text=True).strip()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('image',type=Path);p.add_argument('--compiler',required=True)
    a=p.parse_args();data=a.image.read_bytes()
    result={'artifact':a.image.name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
            'source_commit':output('git','rev-parse','HEAD'),
            'source_dirty':bool(output('git','status','--porcelain','--untracked-files=normal')),
            'compiler':output(a.compiler,'--version').splitlines()[0],
            'hardware_validation':'NOT_RUN','release':False}
    a.image.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
