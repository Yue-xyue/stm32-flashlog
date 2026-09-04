#!/usr/bin/env python3
"""
plot_gc_latency.py — Visualise append-latency tail caused by sector erase.

Reads a CSV produced by `flashlog.py benchmark` and renders a two-panel
figure: the latency time series (showing periodic GC spikes) and the
latency distribution (showing where the tail sits).

Usage:
    python plot_gc_latency.py --csv data/gc_latency_full.csv \
                              --out ../docs/gc_latency.png
"""

import argparse
import csv

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

SPIKE_THRESHOLD_FACTOR = 3.0     # a point > 3x median is treated as a GC spike

COL_BASE  = '#3D6FB4'
COL_SPIKE = '#C4453C'
COL_GREY  = '#8A8A8A'


def read_csv(path):
    idx, lat = [], []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            idx.append(int(row['index']))
            lat.append(float(row['latency_ms']))
    return idx, lat


def percentile(sorted_vals, p):
    n = len(sorted_vals)
    return sorted_vals[min(int(n * p / 100), n - 1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', default='data/gc_latency_full.csv')
    ap.add_argument('--out', default='gc_latency.png')
    ap.add_argument('--payload', type=int, default=32,
                    help='payload size used for the run (for the caption)')
    args = ap.parse_args()

    idx, lat = read_csv(args.csv)
    srt = sorted(lat)
    med = percentile(srt, 50)

    thr = med * SPIKE_THRESHOLD_FACTOR
    spike_i = [i for i, v in zip(idx, lat) if v > thr]
    spike_v = [v for v in lat if v > thr]
    base_v  = [v for v in lat if v <= thr]

    gaps = [b - a for a, b in zip(spike_i, spike_i[1:])]
    gap_txt = f'{gaps[0]}' if gaps and all(g == gaps[0] for g in gaps) else 'varies'

    plt.rcParams.update({
        'font.size': 9,
        'axes.grid': True,
        'grid.alpha': 0.25,
        'grid.linewidth': 0.6,
        'axes.spines.top': False,
        'axes.spines.right': False,
        'axes.titlesize': 10,
        'axes.titleweight': 'bold',
        'legend.frameon': False,
        'figure.dpi': 130,
    })

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.5, 4.3),
                                   gridspec_kw={'width_ratios': [1.75, 1]})

    # ---- (a) time series ------------------------------------------------
    ax1.plot(idx, lat, '.', color=COL_BASE, markersize=2.2,
             label=f'normal append  (n={len(base_v)})')
    ax1.plot(spike_i, spike_v, 'o', color=COL_SPIKE, markersize=5.5,
             label=f'sector erase  (n={len(spike_v)})')

    ax1.axhline(med, color=COL_GREY, linewidth=1.0, linestyle='--')
    ax1.text(len(idx) * 0.995, med * 1.06, f'p50 = {med:.1f} ms',
             ha='right', va='bottom', fontsize=8, color=COL_GREY)

    ax1.set_title('(a)  Append latency over time')
    ax1.set_xlabel('append #')
    ax1.set_ylabel('latency (ms)')
    ax1.set_ylim(0, max(lat) * 1.12)
    ax1.legend(loc='upper left', fontsize=8)
    ax1.text(0.985, 0.66,
             f'spike interval: {gap_txt} appends\n'
             f'= records per 4 KB sector',
             transform=ax1.transAxes, ha='right', va='top',
             fontsize=7.5, color='#555')

    # ---- (b) distribution ----------------------------------------------
    n = len(srt)
    ys = [(i + 1) / n * 100 for i in range(n)]
    ax2.plot(srt, ys, '-', color=COL_BASE, linewidth=1.8)

    p50, p95, p99 = (percentile(srt, p) for p in (50, 95, 99))

    for v, colour in ((p50, COL_GREY), (p95, COL_GREY), (p99, COL_SPIKE)):
        ax2.axvline(v, color=colour, linewidth=1.1,
                    linestyle='-' if colour == COL_SPIKE else ':')

    # percentile values as a text block, so close-together lines stay readable
    ax2.text(0.05, 0.94,
             f'p50   {p50:5.1f} ms\n'
             f'p95   {p95:5.1f} ms\n'
             f'p99   {p99:5.1f} ms\n'
             f'max   {srt[-1]:5.1f} ms',
             transform=ax2.transAxes, ha='left', va='top',
             fontsize=8.5, family='monospace', linespacing=1.5)

    ax2.set_xscale('log')
    ax2.set_title('(b)  Latency distribution (CDF)')
    ax2.set_xlabel('latency (ms, log scale)')
    ax2.set_ylabel('percentile (%)')
    ax2.set_ylim(0, 101)

    ratio = p99 / p95
    ax2.annotate('', xy=(p99, 55), xytext=(p95, 55),
                 arrowprops=dict(arrowstyle='<->', color=COL_SPIKE, linewidth=1.3))
    ax2.text((p95 * p99) ** 0.5, 58, f'×{ratio:.1f}',
             ha='center', va='bottom', fontsize=10,
             color=COL_SPIKE, fontweight='bold')

    fig.suptitle('FlashLog — tail latency introduced by synchronous sector erase',
                 fontsize=12, fontweight='bold', y=0.99)
    fig.tight_layout(rect=(0, 0.02, 1, 0.94))
    fig.savefig(args.out, bbox_inches='tight')
    print(f'wrote {args.out}')

    # ---- console summary ------------------------------------------------
    print(f'\n  samples      : {n}   (payload {args.payload} B)')
    print(f'  p50 / p95    : {percentile(srt,50):.2f} / {percentile(srt,95):.2f} ms')
    print(f'  p99 / max    : {percentile(srt,99):.2f} / {srt[-1]:.2f} ms')
    print(f'  mean         : {sum(lat)/n:.2f} ms')
    print(f'  spikes       : {len(spike_v)}  every {gap_txt} appends')
    if spike_v:
        print(f'  erase cost   : ~{sum(spike_v)/len(spike_v) - med:.1f} ms '
              f'(spike mean - p50)')


if __name__ == '__main__':
    main()
