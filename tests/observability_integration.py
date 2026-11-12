"""Real HTTP / RFC6455 clients, deterministic local crawl, no browser dependency."""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from backpressure_fixture import FanoutFixture

class Client:
    def __init__(self,port):
        self.sock=socket.socket()
        self.sock.settimeout(4)
        self.sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        self.sock.connect(('127.0.0.1',port))
        key=base64.b64encode(b'fluxcrawl-testkey').decode()
        self.sock.sendall(f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
        header=b''
        while not header.endswith(b'\r\n\r\n'): header+=self.sock.recv(1)
        assert b'101 Switching Protocols' in header,header
        accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert accept in header
    def exact(self,n):
        data=b''
        while len(data)<n:
            part=self.sock.recv(n-len(data))
            if not part: raise EOFError('WebSocket closed')
            data+=part
        return data
    def snapshot(self):
        message=bytearray()
        while True:
            first,length=self.exact(2); length &=127
            if length==126: length=struct.unpack('!H',self.exact(2))[0]
            elif length==127: length=struct.unpack('!Q',self.exact(8))[0]
            assert length<200000
            payload=self.exact(length)
            if first&15 in (0,1):
                message.extend(payload)
                assert len(message)<200000
                if first&128:
                    msg=json.loads(message); assert msg['type']=='snapshot'
                    return msg['data']
    def close(self): self.sock.close()

def get(port,path):
    with urllib.request.urlopen(f'http://127.0.0.1:{port}{path}',timeout=4) as response:
        return json.load(response)

def start(binary,site,engine,tmp,extra=()):
    command=[binary,'--engine',engine,'--dashboard','--dashboard-port','0','--metrics-interval-ms','100',
        '--dashboard-linger-ms','1200','--dashboard-topology','--pending-high-watermark','12','--pending-low-watermark','3',
        '--max-per-host','2','--max-pages','1000','--depth','2','--max-retries','1','--retry-backoff-ms','100','--no-retry-jitter']
    if engine=='async': command+=['--concurrency','6']
    if engine=='threaded': command+=['--workers','6']
    command+=list(extra)+[site.url()]
    proc=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    line=proc.stdout.readline(); assert line.startswith('Dashboard: '),(line,proc.stderr.read())
    return proc,int(line.strip().rsplit(':',1)[1])

if __name__=='__main__':
  binary=sys.argv[1]
  for engine in ('sequential','threaded','async'):
    with tempfile.TemporaryDirectory() as tmp, FanoutFixture(6,12,delay=.04 if engine=='sequential' else .012,retry_every=7,duplicates=True) as site:
      extra=['--dedup','bloom-sqlite','--bloom-expected','8','--bloom-fp-rate','0.2','--state-db',tmp+'/crawl.db','--db-batch-size','8'] if engine=='async' else []
      proc,port=start(binary,site,engine,tmp,extra)
      clients=[]
      try:
        clients=[Client(port) for _ in range(4)]
        # Repeated abrupt client exits must not retain sessions or affect work.
        for _ in range(6):
            transient=Client(port); transient.snapshot(); transient.close()
        # Three non-reading clients exercise bounded writes while one consumes.
        states=[]; sequences=set(); saw_active=False; max_payload=0
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            s=clients[0].snapshot(); sequences.add(s['sequence']); states.append(s['backpressure']['active'])
            p=s['progress']; cap=s['engine']['configured_concurrency']
            assert 0<=p['active']<=cap
            saw_active |= p['active']>0
            assert p['seen']>=p['completed']+p['blocked']
            assert s['queues']['pending']==p['pending']
            for h in s['hosts']['items']:
                assert h['active']<=h['per_host_limit']; assert h['pending']>=h['delayed']
            max_payload=max(max_payload,len(json.dumps(s)))
            if s['state'] in ('completed','stopped','error'): break
        else: raise AssertionError('crawl never finished')
        assert s['state']=='completed',s
        assert len(sequences)>2 and saw_active
        assert True in states and states[-1] is False,(engine,states)
        assert s['backpressure']['activations']>0
        assert s['progress']['seen']==site.total
        assert s['hosts']['total_hosts']==3
        assert s['retry']['scheduled']>0 and s['retry']['recovered']>0
        assert len(s['topology']['nodes'])==3
        assert len(s['topology']['edges'])<=128
        if engine=='async':
            bloom=s['dedup']['bloom']; assert bloom['target_fp']==.2
            assert bloom['occupancy']>0 and bloom['false_positives']>0
            assert s['persistence']['transactions']>0 and s['persistence']['average_batch']>0
            assert s['persistence']['commit_ms']>0 and s['persistence']['last_commit_ms']>0
        for path,key in [('/api/status','state'),('/api/metrics','progress'),('/api/hosts','hosts'),('/api/topology','topology')]:
            data=get(port,path); assert data['schema_version']==1 and key in data
        telemetry=get(port,'/api/status')['telemetry']
        assert telemetry['max_inflight_per_client']==1
        # Static production dashboard is served by the same backend, if built.
        assets=Path(__file__).resolve().parents[1]/'dashboard'/'dist'
        if assets.exists():
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/') as response:
                html=response.read().decode(); assert 'FluxCrawl' in html and '/assets/' in html
            for asset in (assets/'assets').iterdir():
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/assets/{asset.name}') as response:
                    assert response.read()==asset.read_bytes()
        # Disconnecting all clients cannot affect the crawler's normal exit.
        for c in clients: c.close()
        clients=[]
        out,err=proc.communicate(timeout=5); assert proc.returncode==0,(out,err)
        assert f'URLs discovered: {site.total}' in out
        print(engine,'live snapshots',len(sequences),'max payload',max_payload,'pressure transitions OK')
      finally:
        for c in clients: c.close()
        if proc.poll() is None: proc.kill();proc.communicate()
  # CLI validation and unavailable port must fail clearly.
  for args in (['--metrics-interval-ms','100'],['--dashboard','--dashboard-port','65536'],['--dashboard','--metrics-interval-ms','0']):
    assert subprocess.run([binary,*args,'http://127.0.0.1/'],capture_output=True).returncode!=0
  with socket.socket() as occupied:
    occupied.bind(('127.0.0.1',0)); occupied.listen()
    result=subprocess.run([binary,'--dashboard','--dashboard-port',str(occupied.getsockname()[1]),'http://127.0.0.1/'],capture_output=True,text=True)
    assert result.returncode!=0 and 'bind' in result.stderr.lower(),result.stderr

  class RobotsFixture(FanoutFixture):
    def response(self,origin,path):
        if path=='/robots.txt': return 200,{'Content-Type':'text/plain'},b'User-agent: *\nDisallow: /leaf/0/\n'
        return super().response(origin,path)
  with tempfile.TemporaryDirectory() as tmp, RobotsFixture(6,12,delay=.015) as site:
    proc,port=start(binary,site,'async',tmp,['--respect-robots'])
    client=Client(port)
    try:
      while True:
        s=client.snapshot()
        if s['state']!='running' and s['state']!='connecting': break
      assert s['robots']['blocked']==12 and s['robots']['fetches']==3,s['robots']
      assert s['progress']['seen']==s['progress']['completed']+s['progress']['blocked']
      client.close()
      out,err=proc.communicate(timeout=5); assert proc.returncode==0,(out,err)
    finally:
      client.close()
      if proc.poll() is None: proc.kill();proc.communicate()

  if len(sys.argv)>2:
    server=subprocess.Popen([sys.argv[2],'--serve'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    clients=[]
    try:
      port=int(server.stdout.readline()); clients=[Client(port) for _ in range(4)]
      # One consuming client and three deliberately non-reading clients.
      deadline=time.monotonic()+7
      while time.monotonic()<deadline:
        clients[0].snapshot()
        telemetry=get(port,'/api/status')['telemetry']
        if telemetry['stalled_disconnects']>0: break
      assert telemetry['dropped_updates']>0 and telemetry['stalled_disconnects']>0,telemetry
      assert telemetry['max_inflight_per_client']==1
      assert telemetry['serialized_snapshots']<100,telemetry
      print('slow clients dropped / disconnected:',telemetry)
    finally:
      for client in clients: client.close()
      server.terminate();server.communicate(timeout=5)
