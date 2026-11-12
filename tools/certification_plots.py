"""Standalone figures from actual certification CSV; never part of C++ build."""
import argparse
import csv
from pathlib import Path
import statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('samples',type=Path);p.add_argument('--output',type=Path,default=Path('docs/figures'));p.add_argument('--advanced',type=Path);p.add_argument('--observability',type=Path);p.add_argument('--soak',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    with a.samples.open() as f:rows=list(csv.DictReader(f))
    profiles=list(dict.fromkeys(r['certification_profile'] for r in rows))
    plt.rcParams.update({'font.size':9,'axes.spines.top':False,'axes.spines.right':False})
    for metric,label,name in [('attempts_per_second','Attempts / second','throughput'),('p95_ms','P95 transfer latency (ms)','latency')]:
        fig,axes=plt.subplots(2,2,figsize=(10,6),constrained_layout=True)
        for ax,profile in zip(axes.flat,profiles):
            for engine,color in [('sequential','#64748b'),('threaded','#0f766e'),('async','#9333ea')]:
                selected=[r for r in rows if r['certification_profile']==profile and r['engine']==engine]
                levels=sorted({int(r['configured_concurrency']) for r in selected})
                values=[statistics.median(float(r[metric]) for r in selected if int(r['configured_concurrency'])==level) for level in levels]
                errors=[statistics.pstdev(float(r[metric]) for r in selected if int(r['configured_concurrency'])==level) for level in levels]
                ax.errorbar(levels,values,yerr=errors,fmt='o-',capsize=3,label=engine,color=color)
            ax.set(title=profile,xlabel='Configured workers / transfers',ylabel=label);ax.set_xticks([1,4,16]);ax.grid(alpha=.2);ax.legend()
        fig.suptitle('FluxCrawl 0.1.0 · Release · medians of '+str(len({r['run'] for r in rows}))+' measured runs')
        fig.savefig(a.output/(name+'.svg'));fig.savefig(a.output/(name+'.png'),dpi=160);plt.close(fig)

    if a.advanced and a.observability:
        import json
        def read(name):
            with (a.advanced/name).open() as f:return list(csv.DictReader(f))
        pressure,persistence,dedup=read('backpressure.csv'),read('persistence.csv'),read('dedup.csv')
        observe=[json.loads(line) for line in a.observability.read_text().splitlines()]
        fig,axes=plt.subplots(2,2,figsize=(10,6),constrained_layout=True)
        def bars(ax,groups,labels,value,title,ylabel):
            values=[statistics.median(value(r) for r in group) for group in groups]
            errors=[statistics.pstdev(value(r) for r in group) for group in groups]
            ax.bar(labels,values,yerr=errors,capsize=3,color='#0f766e',width=.6)
            ax.set(title=title,ylabel=ylabel);ax.grid(axis='y',alpha=.2)
        bars(axes[0,0],[[r for r in pressure if int(r['pending_high'])==n] for n in [0,100,20]],
             ['Off','100 / 50','20 / 5'],lambda r:float(r['peak_pending']),'Queue watermark / peak pending','URLs')
        bars(axes[0,1],[[r for r in persistence if r['batch_size']==n] for n in ['', '1','100','500']],
             ['Off','1','100','500'],lambda r:float(r['process_elapsed_ms']),'Persistence batch threshold','Process elapsed (ms)')
        bars(axes[1,0],[[r for r in dedup if r['mode']==mode and int(r['expected'])==capacity and r['absent_percent']=='95']
                        for mode,capacity in [('memory',100000),('sqlite',100000),('bloom-sqlite',100000),('bloom-sqlite',5000)]],
             ['Memory','SQLite','Bloom\n100k cap','Bloom\n5k cap'],lambda r:float(r['peak_rss_bytes'])/1048576,'Exact dedup: 100k entries','Child peak RSS (MiB)')
        bars(axes[1,1],[[r for r in observe if r['mode']==mode and (r['interval_ms']==500 or mode=='disabled')]
                        for mode in ['disabled','no-clients','client']],
             ['Off','No clients','One client'],lambda r:r['elapsed_s'],'Dashboard at 500 ms','Process elapsed (s)')
        fig.suptitle('Separate feature experiments · medians ± population SD · 3 runs')
        fig.savefig(a.output/'tradeoffs.svg');fig.savefig(a.output/'tradeoffs.png',dpi=160);plt.close(fig)

    if a.soak:
        with a.soak.open() as f:samples=list(csv.DictReader(f,delimiter='\t'))
        fig,axes=plt.subplots(1,2,figsize=(10,3),constrained_layout=True)
        times=[float(s['elapsed_s']) for s in samples]
        axes[0].plot(times,[int(s['rss_bytes'])/1048576 for s in samples],color='#0f766e')
        axes[0].set(xlabel='Elapsed (s)',ylabel='Child RSS after teardown (MiB)',ylim=(0,None))
        axes[1].plot(times,[int(s['descriptors']) for s in samples],color='#64748b')
        axes[1].set(xlabel='Elapsed (s)',ylabel='Descriptors after teardown',ylim=(0,5),yticks=range(6))
        for ax in axes:ax.grid(alpha=.2)
        fig.suptitle('10-minute fixed-graph lifecycle soak · 2,938 sessions · 28 seen URLs/session')
        fig.savefig(a.output/'stability.svg');fig.savefig(a.output/'stability.png',dpi=160);plt.close(fig)
