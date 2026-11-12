import { useEffect, useRef, useState, type ReactNode } from 'react';
import { createRoot } from 'react-dom/client';
import { parseSnapshot, type Snapshot } from './schema';
import './style.css';

const number = (n: number | null | undefined, digits = 0) => n == null ? 'N/A' : n.toLocaleString(undefined, { maximumFractionDigits: digits });
const ms = (n: number | null | undefined) => n == null ? 'N/A' : `${number(n, 1)} ms`;
const pct = (n: number | null | undefined) => n == null ? 'N/A' : `${number(n * 100, 2)}%`;
function Panel({ title, detail, children, wide = false }: { title: string; detail?: string; children: ReactNode; wide?: boolean }) {
  return <section className={`panel ${wide ? 'wide' : ''}`}><div className="panel-head"><h2>{title}</h2>{detail && <span>{detail}</span>}</div>{children}</section>;
}
function Stats({ values }: { values: [string, ReactNode][] }) {
  return <dl>{values.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>)}</dl>;
}
function Chart({ title, history, value, unit, color = '#7ce2bd' }: { title: string; history: Snapshot[]; value: (s: Snapshot) => number; unit: string; color?: string }) {
  const points = history.map(value), max = Math.max(1, ...points);
  const start = history[0]?.elapsed_ms ?? 0, end = history.at(-1)?.elapsed_ms ?? 1;
  const path = history.map((s, i) => `${(s.elapsed_ms - start) / Math.max(1, end - start) * 620 + 5},${110 - points[i] / max * 95}`).join(' ');
  return <Panel title={title} detail={`${number(points.at(-1), 1)} ${unit}`}>
    <svg className="chart" viewBox="0 0 660 140" role="img" aria-label={`${title}, recent history, maximum ${number(max, 1)} ${unit}`}>
      {[15, 62, 110].map(y => <line key={y} x1="5" x2="625" y1={y} y2={y} stroke="#293438" />)}
      <polyline fill="none" stroke={color} strokeWidth="2" points={path}><title>{title}: {number(points.at(-1), 1)} {unit}</title></polyline>
      <text x="635" y="20">{number(max, 0)}</text><text x="5" y="134">{number(start / 1000, 0)}s</text><text x="595" y="134">{number(end / 1000, 0)}s</text>
    </svg>
  </Panel>;
}
function Topology({ snapshot: s }: { snapshot: Snapshot }) {
  const nodes = s.topology.nodes;
  const position = (id: number) => ({ x: 360 + Math.cos(id / Math.max(1, nodes.length) * Math.PI * 2) * 230, y: 145 + Math.sin(id / Math.max(1, nodes.length) * Math.PI * 2) * 100 });
  if (!s.topology.enabled) return <p className="muted">Enable with --dashboard-topology. Tracks the first 64 origins and 128 distinct discovery edges.</p>;
  return <><svg viewBox="0 0 720 290" className="topology" role="img" aria-label="Bounded origin discovery graph">
    {s.topology.edges.map(e => { const a = position(e.source), b = position(e.target); return <line key={`${e.source}-${e.target}`} x1={a.x} x2={b.x} y1={a.y} y2={b.y} stroke="#48635d"><title>{nodes[e.source]?.host} discovered links to {nodes[e.target]?.host}</title></line>; })}
    {nodes.map(n => { const p = position(n.id), h = s.hosts.items.find(h => h.origin === n.host); return <g key={n.id}><circle cx={p.x} cy={p.y} r="10" fill={h?.active ? '#7ce2bd' : '#79969e'}><title>{n.host}: {h ? `${h.active} active` : 'activity unavailable in host sample'}</title></circle><text x={p.x} y={p.y + 27} textAnchor="middle">{n.host.replace('http://', '')}</text></g>; })}
  </svg><p className="muted">{nodes.length}/64 origins · {s.topology.edges.length}/128 distinct edges · first discovered, directed source → target (see tooltips)</p></>;
}
function App() {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [connection, setConnection] = useState('Connecting');
  const [history, setHistory] = useState<Snapshot[]>([]);
  const lastSequence = useRef(-1);
  useEffect(() => {
    let socket: WebSocket | undefined, retry: ReturnType<typeof setTimeout> | undefined, disposed = false, delay = 1000;
    const connect = () => {
      if (disposed) return;
      socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
      socket.onopen = () => { setConnection('Connected'); delay = 1000; lastSequence.current = -1; };
      socket.onmessage = event => {
        const s = parseSnapshot(String(event.data));
        if (!s || s.sequence === lastSequence.current) return;
        lastSequence.current = s.sequence; setSnapshot(s);
        setHistory(previous => [...(previous.at(-1) && s.elapsed_ms < previous.at(-1)!.elapsed_ms ? [] : previous), s].filter(p => s.elapsed_ms - p.elapsed_ms <= 300000).slice(-3000));
      };
      socket.onerror = () => socket?.close();
      socket.onclose = () => { if (!disposed) { setConnection('Disconnected'); retry = setTimeout(connect, delay); delay = Math.min(15000, delay * 2); } };
    };
    connect(); return () => { disposed = true; clearTimeout(retry); socket?.close(); };
  }, []);
  const s = snapshot;
  const state = connection !== 'Connected' ? connection : s ? s.state : 'Connecting';
  return <main>
    <header><div className="brand"><span className="mark">F/</span><div><h1>FluxCrawl</h1><p>RUNTIME OBSERVATORY</p></div></div><div className="session"><span className="chip">{s?.engine.name || '—'}</span><span className={`chip ${state === 'running' ? 'live' : ''}`} role="status">{state}</span>{s && connection !== 'Connected' && <span className="chip">{s.state}</span>}<time>{s ? new Date(s.elapsed_ms).toISOString().slice(11, 19) : '00:00:00'}</time></div></header>
    <div className="intro"><h2>Inside the crawl</h2><p>Request flow, scheduling pressure, and storage — as they happen.</p></div>
    {!s || !s.engine.name ? <section className="panel empty">Waiting for the first crawler snapshot… <p>WebSocket · /ws · schema v1</p></section> : <>
      <div className="summary">{[
        ['Rolling throughput', `${number(s.throughput.rolling_rps, 1)}`, 'requests / sec · 5s'],
        ['Active requests', number(s.progress.active), `of ${s.engine.configured_concurrency} slots`],
        ['Pending URLs', number(s.progress.pending), `${number(s.queues.deferred)} deferred on disk`],
        ['Logical URLs seen', number(s.progress.seen), `${number(s.progress.completed)} completed`],
        ['Recent P95 latency', ms(s.latency.p95_ms), '5s · at most 4,096 samples'],
      ].map(([label, value, note]) => <section key={label}><h3>{label}</h3><strong>{value}</strong><p>{note}</p></section>)}</div>
      <div className="grid">
        <Chart title="01 / Throughput" history={history} value={s => s.throughput.rolling_rps} unit="req/s" />
        <Chart title="02 / Active requests" history={history} value={s => s.progress.active} unit="active" color="#9db7ed" />
        <Chart title="03 / Pending queue" history={history} value={s => s.progress.pending} unit="URLs" color="#e4bd7d" />
        <Chart title="04 / Recent P95 latency" history={history} value={s => s.latency.p95_ms ?? 0} unit="ms" color="#beabe4" />
        <Panel title="Queue pressure" detail={s.backpressure.active ? 'BACKPRESSURED' : 'NORMAL'}>
          <progress aria-label="Admitted pending queue relative to high watermark" max={s.backpressure.high} value={s.backpressure.pending} />
          <Stats values={[[ 'High / low watermarks', `${number(s.backpressure.high)} / ${number(s.backpressure.low)}`],['Peak pending',number(s.backpressure.peak_pending)],['Activations',number(s.backpressure.activations)],['Time under pressure',ms(s.backpressure.time_ms)],['Admission control',s.backpressure.enabled ? 'Enabled' : 'Disabled']]} />
        </Panel>
        <Panel title="Retries & robots" detail={`${s.retry.delayed} delayed (incl. robots)`}>
          <Stats values={[[ 'Retries scheduled / attempted',`${s.retry.scheduled} / ${s.retry.attempted}`],['Recovered / exhausted',`${s.retry.recovered} / ${s.retry.exhausted}`],['Next eligibility',ms(s.retry.next_eligible_ms)],['Robots fetches / cache hits',`${s.robots.fetches} / ${s.robots.cache_hits}`],['Blocked / policy failures',`${s.robots.blocked} / ${s.robots.failures}`],['URLs / hosts waiting for robots',`${number(s.robots.held_urls)} / ${number(s.robots.waiting_hosts)}`]]} />
        </Panel>
        <Panel title="Host activity" detail={`${s.hosts.items.length} shown / ${s.hosts.total_hosts} origins`} wide>
          <div className="table-wrap"><table><thead><tr>{['Origin', 'Active / limit', 'Pending', 'Completed', 'Delayed', 'Rate / burst', 'Rate wait'].map(x => <th key={x}>{x}</th>)}</tr></thead><tbody>{s.hosts.items.map(h => <tr key={h.origin}><td>{h.origin}</td><td>{h.active} / {h.per_host_limit}</td><td>{number(h.pending)}</td><td>{number(h.completed)}</td><td>{h.delayed}</td><td>{h.rate_rps == null ? 'Unlimited' : `${number(h.rate_rps, 1)}/s · ${h.burst}`}</td><td>{ms(h.wait_ms)}</td></tr>)}</tbody></table></div><p className="muted">{s.hosts.selection}. Pending includes delayed work; robots-held URLs appear above.</p>
        </Panel>
        <Panel title="Exact deduplication" detail={s.dedup.backend}>
          <Stats values={[[ 'Seen / duplicates',`${number(s.dedup.seen)} / ${number(s.dedup.duplicates)}`],['DB membership queries',number(s.dedup.exact_db_queries)],['Total lookup time',ms(s.dedup.lookup_ms)]]} />
          {s.dedup.bloom && <><h3>Bloom accelerator</h3><progress aria-label="Bloom occupancy" max={1} value={s.dedup.bloom.occupancy} /><Stats values={[[ 'Occupancy / bytes',`${pct(s.dedup.bloom.occupancy)} / ${number(s.dedup.bloom.bytes)}`],['Inserted / design capacity',`${number(s.dedup.bloom.inserted)} / ${number(s.dedup.bloom.expected)}`],['Target / observed FP',`${pct(s.dedup.bloom.target_fp)} / ${pct(s.dedup.bloom.observed_fp)}`],['False positives / exact checks',`${number(s.dedup.bloom.false_positives)} / ${number(s.dedup.bloom.exact_checks)}`],['Definite negatives',number(s.dedup.bloom.definite_negatives)]]} /></>}
        </Panel>
        <Panel title="Persistence" detail={s.persistence.mode}>
          {s.persistence.mode === 'off' ? <p className="muted">Persistence is off for this crawl.</p> : <Stats values={s.persistence.mode === 'sqlite' ? [[ 'Transactions',number(s.persistence.transactions)],['Pending mutations',number(s.persistence.pending_mutations)],['Average batch size',number(s.persistence.average_batch, 1)],['Last / total commit time',`${ms(s.persistence.last_commit_ms)} / ${ms(s.persistence.commit_ms)}`],['Database bytes (final only)',number(s.persistence.database_bytes)]] : [['Checkpoints',number(s.persistence.checkpoints)],['Last checkpoint',s.persistence.last_checkpoint_unix_ms ? new Date(s.persistence.last_checkpoint_unix_ms).toLocaleTimeString() : 'N/A'],['Last checkpoint size',`${number(s.persistence.checkpoint_bytes)} bytes`],['Last / total checkpoint time',`${ms(s.persistence.last_checkpoint_ms)} / ${ms(s.persistence.checkpoint_ms)}`]]} />}
        </Panel>
        <Panel title="Engine & progress" detail={s.engine.name}>
          <Stats values={[[ 'Fetch attempts / responses',`${number(s.progress.attempts)} / ${number(s.progress.responses)}`],['Successful 2xx (session)',number(s.progress.successful_responses_session)],['Fetch failures',number(s.progress.fetch_failures)],['Session average throughput',`${number(s.throughput.average_rps, 1)} req/s`],['Recent P50 / P99 / mean',`${ms(s.latency.p50_ms)} / ${ms(s.latency.p99_ms)} / ${ms(s.latency.mean_ms)}`],['Maximum active',number(s.engine.max_active)],...(s.engine.name === 'threaded' ? [[ 'Active / idle workers',`${s.engine.active_workers} / ${s.engine.idle_workers}`],['Worker waits',number(s.engine.worker_waits)],['Lock wait time',ms(s.engine.lock_wait_ms)]] as [string, ReactNode][] : s.engine.name === 'async' ? [['Event-loop iterations',number(s.engine.iterations)],['Event-loop wait time',ms(s.engine.wait_ms)],['Transfer completions',number(s.engine.completions)]] as [string, ReactNode][] : [])]} />
        </Panel>
        <Panel title="Recent errors" detail={`${s.errors.length} retained`}><ul className="errors">{s.errors.length ? s.errors.map((e, i) => <li key={i}>{e}</li>) : <li className="muted">No recent errors.</li>}</ul></Panel>
        <Panel title="Origin discovery topology" detail="Bounded sample" wide><Topology snapshot={s} /></Panel>
      </div>
    </>}
    <footer><span>FLUXCRAWL / LOCAL SYSTEMS TELEMETRY</span><span>Read-only · sampled · best-effort · 5 minute chart history</span></footer>
  </main>;
}
createRoot(document.getElementById('root')!).render(<App />);
