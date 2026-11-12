export interface Host {
  origin: string; pending: number; active: number; delayed: number; completed: number;
  per_host_limit: number; rate_rps: number | null; burst: number | null; wait_ms: number;
}
export interface Snapshot {
  schema_version: 1; sequence: number; state: 'connecting' | 'running' | 'completed' | 'stopped' | 'error';
  running: boolean; elapsed_ms: number;
  engine: { name: string; configured_concurrency: number; max_active: number; workers?: number;
    active_workers?: number; idle_workers?: number; worker_waits?: number; lock_wait_ms?: number;
    active_transfers?: number; iterations?: number; wait_ms?: number; completions?: number };
  progress: { seen: number; pending: number; active: number; completed: number; blocked: number;
    attempts: number; responses: number; successful_responses_session: number; fetch_failures: number };
  throughput: { rolling_rps: number; average_rps: number; window_ms: number; basis: string };
  latency: { scope: string; samples: number; p50_ms: number | null; p95_ms: number | null;
    p99_ms: number | null; mean_ms: number | null };
  queues: { pending: number; delayed: number; robots_held: number; deferred: number };
  retry: { delayed: number; scheduled: number; attempted: number; exhausted: number; recovered: number; next_eligible_ms: number | null };
  robots: { fetches: number; cache_hits: number; blocked: number; failures: number; held_urls: number; waiting_hosts: number };
  backpressure: { enabled: boolean; active: boolean; pending: number; high: number; low: number;
    peak_pending: number; activations: number; time_ms: number };
  dedup: { backend: string; seen: number; duplicates: number; lookup_ms: number; exact_db_queries: number;
    bloom?: { bytes: number; occupancy: number; target_fp: number; observed_fp: number | null;
      inserted: number; expected: number; definite_negatives: number; maybe_hits: number; false_positives: number; exact_checks: number } };
  persistence: { mode: string; transactions: number; pending_mutations: number; average_batch: number;
    commit_ms: number; last_commit_ms: number; database_bytes: number; checkpoints: number;
    checkpoint_bytes: number; checkpoint_ms: number; last_checkpoint_ms: number; last_checkpoint_unix_ms: number };
  hosts: { total_hosts: number; candidates: number; selection: string; items: Host[] };
  topology: { enabled: boolean; kind: string; max_nodes: number; max_edges: number;
    nodes: { id: number; host: string }[]; edges: { source: number; target: number }[] };
  errors: string[];
}
// External telemetry must be structurally safe before React maps or renders it.
type Data = Record<string, unknown>;
const object = (v: unknown): v is Data => typeof v === 'object' && v !== null && !Array.isArray(v);
const number = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v) && v >= 0;
const nullable = (v: unknown) => v === null || number(v);
const numbers = (v: unknown, keys: string): v is Data => object(v) && keys.split(' ').every(k => number(v[k]));
export function parseSnapshot(message: string): Snapshot | null {
  try {
    const e: unknown = JSON.parse(message);
    if (!object(e) || e.type !== 'snapshot' || !object(e.data)) return null;
    const s=e.data;
    if (s.schema_version !== 1 || !number(s.sequence) || !number(s.elapsed_ms) || typeof s.running !== 'boolean' ||
        !['connecting','running','completed','stopped','error'].includes(String(s.state))) return null;
    if (!numbers(s.engine,'configured_concurrency max_active') || !['','sequential','threaded','async'].includes(String(s.engine.name))) return null;
    for (const [key,value] of Object.entries(s.engine)) { if (key !== 'name' && !number(value)) return null; }
    const groups: Record<string,string> = {
      progress:'seen pending active completed blocked attempts responses successful_responses_session fetch_failures',
      throughput:'rolling_rps average_rps window_ms', latency:'samples', queues:'pending delayed robots_held deferred',
      retry:'delayed scheduled attempted exhausted recovered', robots:'fetches cache_hits blocked failures held_urls waiting_hosts',
      backpressure:'pending high low peak_pending activations time_ms', dedup:'seen duplicates lookup_ms exact_db_queries',
      persistence:'transactions pending_mutations average_batch commit_ms last_commit_ms database_bytes checkpoints checkpoint_bytes checkpoint_ms last_checkpoint_ms last_checkpoint_unix_ms',
      hosts:'total_hosts candidates',topology:'max_nodes max_edges'
    };
    for (const [key,keys] of Object.entries(groups)) { if (!numbers(s[key],keys)) return null; }
    const latency=s.latency as Data, retry=s.retry as Data, pressure=s.backpressure as Data;
    const dedup=s.dedup as Data, hosts=s.hosts as Data, topology=s.topology as Data;
    if (typeof (s.throughput as Data).basis !== 'string' || typeof latency.scope !== 'string' ||
        !['p50_ms','p95_ms','p99_ms','mean_ms'].every(k=>nullable(latency[k])) || !nullable(retry.next_eligible_ms) ||
        typeof pressure.enabled !== 'boolean' || typeof pressure.active !== 'boolean' || typeof dedup.backend !== 'string') return null;
    if (dedup.bloom !== undefined && (!numbers(dedup.bloom,'bytes occupancy target_fp inserted expected definite_negatives maybe_hits false_positives exact_checks') || !nullable(dedup.bloom.observed_fp))) return null;
    if (!['off','sqlite','snapshot'].includes(String((s.persistence as Data).mode))) return null;
    if (typeof hosts.selection !== 'string' || !Array.isArray(hosts.items) || hosts.items.length>32 ||
        !hosts.items.every(h=>numbers(h,'pending active delayed completed per_host_limit wait_ms') && typeof h.origin === 'string' && nullable(h.rate_rps) && nullable(h.burst))) return null;
    if (typeof topology.enabled !== 'boolean' || typeof topology.kind !== 'string' || !Array.isArray(topology.nodes) || !Array.isArray(topology.edges) || topology.nodes.length>64 || topology.edges.length>128) return null;
    const ids=new Set<number>();
    for (const n of topology.nodes) { if (!object(n) || !number(n.id) || !Number.isInteger(n.id) || typeof n.host !== 'string' || ids.has(n.id)) return null; ids.add(n.id); }
    if (!topology.edges.every(edge=>object(edge) && typeof edge.source === 'number' && typeof edge.target === 'number' && ids.has(edge.source) && ids.has(edge.target))) return null;
    if (!Array.isArray(s.errors) || s.errors.length>25 || !s.errors.every(error=>typeof error === 'string')) return null;
    return s as unknown as Snapshot;
  } catch { return null; }
}
