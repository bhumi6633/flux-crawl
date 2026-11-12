"""Explicit deterministic HTTP faults. Only loopback and test-sized responses."""
from collections import Counter
from http.server import BaseHTTPRequestHandler
import socket
import threading
import time
from local_http import LocalHttpServer
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_GET(self):
        prefix,_,name=self.path.rpartition('/')
        with self.server.lock:
            self.server.counts[self.path]+=1;attempt=self.server.counts[self.path]
        if name=='close' and attempt==1:
            self.close_connection=True
            self.connection.shutdown(socket.SHUT_RDWR);return
        if name=='slow' and attempt==1:time.sleep(.20)
        if name=='partial' and attempt==1:
            self.send_response(200);self.send_header('Content-Length','100');self.end_headers()
            self.wfile.write(b'short');self.wfile.flush();self.close_connection=True;return
        status=200;headers={};body=b'ok'
        if name=='root':
            cases='partial close slow retry429 retry503 always500 loop-a chain-0 missing oversized malformed good'.split()
            body=''.join(f'<a href="{prefix}/{case}">{case}</a>' for case in cases).encode()
            headers['Content-Type']='text/html'
        elif name in ('retry429','retry503') and attempt==1:status=int(name[5:])
        elif name=='always500':status=500
        elif name in ('loop-a','loop-b'):status=302;headers['Location']=prefix+('/loop-b' if name=='loop-a' else '/loop-a')
        elif name.startswith('chain-'):
            n=int(name[6:]);status=302 if n<2 else 200
            if n<2:headers['Location']=f'{prefix}/chain-{n+1}'
        elif name=='oversized':body=b'x'*262144
        elif name=='malformed':
            headers['Content-Type']='text/html';body=b'<div>'*100+b'<a href="good#x">x'*20+b'<a href="">empty'
        self.send_response(status)
        self.send_header('Content-Length',str(len(body)))
        if name!='missing':self.send_header('Content-Type',headers.pop('Content-Type','text/plain'))
        for key,value in headers.items():self.send_header(key,value)
        self.end_headers()
        try:self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):pass
class ChaosSite:
    def __enter__(self):
        self.server=LocalHttpServer(('127.0.0.1',0),Handler);self.server.counts=Counter();self.server.lock=threading.Lock()
        self.thread=threading.Thread(target=self.server.serve_forever,kwargs={'poll_interval':.01});self.thread.start();return self
    def url(self):return f'http://127.0.0.1:{self.server.server_port}'
    def __exit__(self,*args):self.server.shutdown();self.server.server_close();self.thread.join()
