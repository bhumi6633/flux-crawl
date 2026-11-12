"""Create a deterministic SOURCE release from committed files; no network/publish/tag."""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re
import subprocess
import tarfile
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    if subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip():p.error('commit or remove outstanding changes before packaging')
    header=(root/'include/fluxcrawl/version.hpp').read_text();version=re.search(r'version = "([^"]+)"',header)[1]
    if a.output.exists():p.error('output must not exist')
    data=subprocess.check_output(['git','archive','--format=tar','--prefix=fluxcrawl-'+version+'/','HEAD'],cwd=root)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        names=archive.getnames()
        if any('/node_modules/' in n or '/build/' in n or '/.git/' in n or n.endswith(('.db','.chk')) for n in names):raise RuntimeError('unexpected generated release content')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with a.output.open('xb') as file:
        with gzip.GzipFile(filename='',mode='wb',fileobj=file,mtime=0) as compressed:compressed.write(data)
    digest=hashlib.sha256(a.output.read_bytes()).hexdigest()
    a.output.with_suffix(a.output.suffix+'.sha256').write_text(digest+'  '+a.output.name+'\n')
    print(a.output.name,digest)
