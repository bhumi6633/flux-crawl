import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseSnapshot } from './schema.ts';
const counts=(keys:string)=>Object.fromEntries(keys.split(' ').map(k=>[k,0]));
function sample() { return {type:'snapshot',data:{
  schema_version:1,sequence:1,elapsed_ms:20,state:'running',running:true,
  engine:{name:'async',configured_concurrency:8,max_active:1},
  progress:counts('seen pending active completed blocked attempts responses successful_responses_session fetch_failures'),
  throughput:{...counts('rolling_rps average_rps window_ms'),basis:'session'},
  latency:{samples:0,scope:'recent',p50_ms:null,p95_ms:null,p99_ms:null,mean_ms:null},
  queues:counts('pending delayed robots_held deferred'),
  retry:{...counts('delayed scheduled attempted exhausted recovered'),next_eligible_ms:null},
  robots:counts('fetches cache_hits blocked failures held_urls waiting_hosts'),
  backpressure:{...counts('pending high low peak_pending activations time_ms'),enabled:true,active:false},
  dedup:{...counts('seen duplicates lookup_ms exact_db_queries'),backend:'memory'},
  persistence:{...counts('transactions pending_mutations average_batch commit_ms last_commit_ms database_bytes checkpoints checkpoint_bytes checkpoint_ms last_checkpoint_ms last_checkpoint_unix_ms'),mode:'off'},
  hosts:{total_hosts:0,candidates:0,selection:'sample',items:[]},
  topology:{enabled:false,kind:'origins',max_nodes:64,max_edges:128,nodes:[],edges:[]},errors:[]
}}; }
test('valid v1 tolerates absent Bloom data',()=>{const s=parseSnapshot(JSON.stringify(sample()));assert.ok(s);assert.equal(s.dedup.bloom,undefined);});
test('malformed and incompatible envelopes reject',()=>{
  for(const text of ['', '{','null','[]','{}','{"type":"snapshot","data":null}'])assert.equal(parseSnapshot(text),null);
  const s=sample();s.data.schema_version=2;assert.equal(parseSnapshot(JSON.stringify(s)),null);
});
test('malformed nested arrays and dangling graph edges reject',()=>{
  for(const patch of [{hosts:{items:null}},{progress:{seen:-1}},{topology:{nodes:[],edges:[{source:0,target:1}]}},{errors:[{}]}]){
    const s=sample();Object.assign(s.data,patch);assert.equal(parseSnapshot(JSON.stringify(s)),null);
  }
  assert.equal(parseSnapshot(JSON.stringify(sample()).replace('"sequence":1','"sequence":1e999')),null);
});
test('unknown top-level fields remain compatible',()=>{const s=sample();Object.assign(s.data,{future_metric:42});assert.ok(parseSnapshot(JSON.stringify(s)));});
