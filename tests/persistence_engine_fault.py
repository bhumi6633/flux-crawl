import subprocess
import sys
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_fixture import FanoutFixture
with tempfile.TemporaryDirectory() as tmp,FanoutFixture(3,4,delay=.001,duplicates=True) as site:
    subprocess.run([sys.argv[1],site.url(),tmp],check=True,timeout=30)
