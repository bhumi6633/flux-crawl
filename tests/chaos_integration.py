import subprocess
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from chaos_fixture import ChaosSite
with ChaosSite() as site:
    subprocess.run([sys.argv[1],site.url()],check=True,timeout=20)
    with site.server.lock:counts=dict(site.server.counts)
    for engine in range(3):
        prefix=f'/{engine}/'
        assert counts[prefix+'oversized']==1
        assert counts[prefix+'partial']==2 and counts[prefix+'slow']==2
        assert counts[prefix+'good']==1
        assert counts[prefix+'always500']==2
