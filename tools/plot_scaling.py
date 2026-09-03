#!/usr/bin/env python3
"""
plot_scaling.py — Visualise FlashLog mount-time scaling measurements.
 
Reads the CSV files produced by `flashlog.py scaling` and renders a
three-panel figure summarising the optimisation study.
 
Usage:
    python plot_scaling.py [--indir data] [--out mount_scaling.png]
 
Expected CSV files (records,init_us,used_bytes,us_per_record):
    s4.csv        s64.csv        - v1: bit-wise CRC32, SPI @ 1 MHz
    s4_table.csv  s64_table.csv  - v2: table-driven CRC32, SPI @ 1 MHz
    s4_new.csv    s64_new.csv    - v3: table-driven CRC32, SPI @ 8 MHz
"""
 
import argparse
import csv
import os
 
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
 
 
# ---------------------------------------------------------------- data model
 
VERSIONS = [
    # key,      label,                              4B file,         64B file,        colour
    ('v1', 'v1  bit-wise CRC · SPI 1 MHz', 's4.csv',       's64.csv',       '#C4453C'),
    ('v2', 'v2  table CRC · SPI 1 MHz',    's4_table.csv', 's64_table.csv', '#3D6FB4'),
    ('v3', 'v3  table CRC · SPI 8 MHz',    's4_new.csv',   's64_new.csv',   '#2E8B57'),
]
 
PAYLOAD_SMALL = 4
PAYLOAD_LARGE = 64
 
 
def read_csv(path):
    """Return (records[], mount_ms[], us_per_record[])."""
    n, ms, per = [], [], []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            n.append(int(row['records']))
            ms.append(int(row['init_us']) / 1000.0)
            per.append(float(row['us_per_record']))
    return n, ms, per
 
 
def linear_model(cost_small, cost_large):
    """Fit t = intercept + slope * payload through two measured points."""
    slope = (cost_large - cost_small) / (PAYLOAD_LARGE - PAYLOAD_SMALL)
    intercept = cost_small - slope * PAYLOAD_SMALL
    return intercept, slope
 
 
# ------------------------------------------------------------------ plotting
 
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--indir', default='.', help='directory holding the CSV files')
    ap.add_argument('--out', default='mount_scaling.png')
    args = ap.parse_args()
 
    data = {}
    for key, label, f_small, f_large, colour in VERSIONS:
        data[key] = {
            'label': label,
            'colour': colour,
            'small': read_csv(os.path.join(args.indir, f_small)),
            'large': read_csv(os.path.join(args.indir, f_large)),
        }
 
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
 
    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(13.5, 4.3))
 
    # ---- (a) mount time vs record count, 64 B payload ---------------------
    for key, d in data.items():
        n, ms, _ = d['large']
        ax1.plot(n, ms, 'o-', color=d['colour'], label=d['label'],
                 markersize=4.5, linewidth=1.8)
 
    ax1.set_title('(a)  Mount time is O(n)')
    ax1.set_xlabel('records in log')
    ax1.set_ylabel('mount time (ms)')
    ax1.set_xlim(0, 110)
    ax1.set_ylim(0, None)
    ax1.legend(loc='upper left', fontsize=8)
    ax1.text(0.97, 0.05,
             '64 B payload\nlinearity error < 1 %',
             transform=ax1.transAxes, ha='right', va='bottom',
             fontsize=7.5, color='#555')
 
    # ---- (b) per-record cost model ---------------------------------------
    for key, d in data.items():
        c_small = d['small'][2][-1]     # us/record at largest n
        c_large = d['large'][2][-1]
        b, m = linear_model(c_small, c_large)
 
        xs = [0, 70]
        ax2.plot(xs, [b + m * x for x in xs], '-', color=d['colour'], linewidth=1.6)
        ax2.plot([PAYLOAD_SMALL, PAYLOAD_LARGE], [c_small, c_large], 'o',
                 color=d['colour'], markersize=5.5,
                 label=f'{key}:  {b:.0f} + {m:.1f}·s  µs')
 
    ax2.set_title('(b)  Cost model per record')
    ax2.set_xlabel('payload size s (bytes)')
    ax2.set_ylabel('cost per record (µs)')
    ax2.set_xlim(0, 70)
    ax2.set_ylim(0, None)
    ax2.legend(loc='upper left', fontsize=8)
 
    # ---- (c) where the variable cost went --------------------------------
    c1s, c1l = data['v1']['small'][2][-1], data['v1']['large'][2][-1]
    c2s, c2l = data['v2']['small'][2][-1], data['v2']['large'][2][-1]
    c3s, c3l = data['v3']['small'][2][-1], data['v3']['large'][2][-1]
 
    _, m1 = linear_model(c1s, c1l)
    _, m2 = linear_model(c2s, c2l)
    _, m3 = linear_model(c3s, c3l)
 
    crc_part = m1 - m2          # saved by table-driven CRC
    spi_part = m2 - m3          # saved by raising SPI clock
    hal_part = m3               # what remains
 
    parts = [
        ('CRC32 algorithm\n(bit-wise → table)', crc_part, '#C4453C'),
        ('SPI transfer\n(1 → 8 MHz)',           spi_part, '#3D6FB4'),
        ('HAL per-byte overhead\n(unexplained)', hal_part, '#8A8A8A'),
    ]
 
    bottom = 0.0
    for name, value, colour in parts:
        ax3.bar(0, value, bottom=bottom, width=0.5, color=colour,
                edgecolor='white', linewidth=1.2)
        ax3.text(0.32, bottom + value / 2,
                 f'{name}\n{value:.1f} µs/B  ({value / m1 * 100:.0f} %)',
                 va='center', ha='left', fontsize=8)
        bottom += value
 
    ax3.set_title('(c)  Variable cost decomposition')
    ax3.set_ylabel('µs per payload byte')
    ax3.set_xlim(-0.4, 1.9)
    ax3.set_ylim(0, m1 * 1.12)
    ax3.set_xticks([0])
    ax3.set_xticklabels(['v1 total\n%.1f µs/B' % m1])
    ax3.grid(axis='x', visible=False)
 
    fig.suptitle('FlashLog — mount-time scaling and optimisation study',
                 fontsize=12, fontweight='bold', y=0.99)
    fig.tight_layout(rect=(0, 0.02, 1, 0.95))
    fig.savefig(args.out, bbox_inches='tight')
    print(f'wrote {args.out}')
 
    # ---- console summary --------------------------------------------------
    print('\ncost model  t(n,s) = n * (a + b*s)  µs')
    for key, d in data.items():
        b, m = linear_model(d['small'][2][-1], d['large'][2][-1])
        print(f'  {key}:  a = {b:7.1f} µs    b = {m:5.2f} µs/B')
    print(f'\nvariable cost reduction: {m1:.1f} -> {m3:.1f} µs/B '
          f'({(1 - m3 / m1) * 100:.0f} %)')
 
 
if __name__ == '__main__':
    main()