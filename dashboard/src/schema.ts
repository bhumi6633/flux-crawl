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
// Reject wrong schema/envelope; backend owns numeric field validation.
export function parseSnapshot(message: string): Snapshot | null {
  try {
    const envelope: unknown = JSON.parse(message);
    if (typeof envelope !== 'object' || !envelope || !('type' in envelope) || envelope.type !== 'snapshot' || !('data' in envelope)) return null;
    const s = envelope.data as Partial<Snapshot> | null;
    if (!s || s.schema_version !== 1 || typeof s.sequence !== 'number' || !s.progress || !s.engine || !s.throughput || !s.latency || !s.queues || !s.retry || !s.robots || !s.backpressure || !s.dedup || !s.persistence || !s.hosts || !s.topology || !Array.isArray(s.errors)) return null;
    return s as Snapshot;
  } catch { return null; }
}
