"""Generate publication/export-friendly standalone figures from recorded results."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); args=p.parse_args()
rows=json.loads((args.directory/'results.json').read_text()); dest=args.directory/'figures'; dest.mkdir(exist_ok=True)
plt.rcParams.update({'font.size':10,'axes.grid':True,'grid.alpha':.25,'svg.fonttype':'none'})
def save(fig,name):
    fig.tight_layout(); fig.savefig(dest/(name+'.svg')); fig.savefig(dest/(name+'.png'),dpi=160); plt.close(fig)
def group(g): return [r for r in rows if r['group']==g]
fig,ax=plt.subplots(figsize=(7,4))
for mode in ('serial','paired','double'):
    rs=[r for r in group('E1') if r['schedule']==mode]
    ax.plot([r['M'] for r in rs],[next(x['sim_s'] for x in group('E1') if x['M']==r['M'] and x['schedule']=='serial')/r['sim_s'] for r in rs],'o-',label=mode)
ax.set(xlabel='Cubic GEMM dimension',ylabel='Speedup over serial',xscale='log',title='Input concurrency and scheduling overlap'); ax.legend(); save(fig,'schedule')
fig,axes=plt.subplots(1,2,figsize=(10,4)); rs=group('E2'); x=[r['interconnect_bw_GBps'] for r in rs]
axes[0].plot(x,[r['useful_ops_s']/1e9 for r in rs],'o-'); axes[0].set(ylabel='Useful GOP/s')
axes[1].plot(x,[r['avg_queue_ns'] for r in rs],'o-'); axes[1].set(ylabel='Mean queue delay (ns)')
for ax in axes: ax.set(xlabel='IC bandwidth (GB/s)',xscale='log',title='128 x 128 x 128, array 16')
save(fig,'bandwidth')
fig,axes=plt.subplots(1,2,figsize=(10,4))
for dim in (32,128):
 for bw in (32,256):
    rs=[r for r in group('E3') if r['M']==dim and r['interconnect_bw_GBps']==bw]
    label=f'GEMM {dim}, IC {bw} GB/s'
    axes[0].plot([r['array_n'] for r in rs],[r['useful_ops_s']/1e9 for r in rs],'o-',label=label)
    axes[1].plot([r['array_n'] for r in rs],[r['bytes']/1024 for r in rs],'o-',label=label)
axes[0].set(ylabel='Useful GOP/s'); axes[1].set(ylabel='HBM traffic (KiB)',yscale='log')
for ax in axes: ax.set(xlabel='Array side',title='Array and tiling change together')
axes[0].legend(fontsize=8); save(fig,'array')
fig,axes=plt.subplots(1,2,figsize=(10,4))
for ax,bw in zip(axes,(16,256)):
    rs=[r for r in group('E4') if r['interconnect_bw_GBps']==bw]
    xs=[2**(i/10) for i in range(-10,61)]
    peak=512; ceiling=peak/3
    ax.plot(xs,[min(peak,bw*x) for x in xs],label='Nominal roof (steady-state IC)')
    ax.axhline(ceiling,color='gray',linestyle='--',label='PE timing ceiling')
    for idx,r in enumerate(rs):
        ax.scatter(r['useful_ai'],r['useful_ops_s']/1e9)
        ax.annotate('K='+str(r['K']),(r['useful_ai'],r['useful_ops_s']/1e9),fontsize=8,xytext=(-38,12 + 12*idx),textcoords='offset points',arrowprops={'arrowstyle':'-', 'lw':.5})
    ax.set(xscale='log',yscale='log',xlabel='Useful OP / HBM Byte',ylabel='Useful GOP/s',title=f'Array 16, IC {bw} GB/s'); ax.legend(fontsize=7)
save(fig,'roofline')
fig,axes=plt.subplots(1,2,figsize=(10,4));rs=group('E5')
axes[0].plot([r['buffer_kb'] for r in rs],[r['bytes']/1024 for r in rs],'o-');axes[0].set(ylabel='HBM traffic (KiB)')
axes[1].plot([r['buffer_kb'] for r in rs],[r['sim_s']*1e6 for r in rs],'o-');axes[1].set(ylabel='Simulation time (us)')
for ax in axes: ax.set(xlabel='Buffer capacity (KiB)',xscale='log',title='Fixed tiling: capacity does not drive reuse')
save(fig,'buffer')
print('Saved five SVG/PNG figure pairs to',dest)
