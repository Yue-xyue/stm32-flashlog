"""FlashLog host-side CLI"""

import argparse
import csv
import sys
import time

import serial


class FlashLog:
    """封裝與板子的通訊"""

    def __init__(self, port, baud=115200, timeout=5.0):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.timeout = timeout
        time.sleep(0.1)
        self.ser.reset_input_buffer()   # 清掉開機訊息等殘留

    def close(self):
        self.ser.close()

    def send(self, cmd, verbose=False):
        """送一個指令，讀到 OK/ERR 為止。

        回傳 (ok, code, lines)
          ok    : True/False
          code  : 錯誤碼（成功為 0）
          lines : 中間的輸出（已去掉 echo 與結束標記）
        """
        self.ser.reset_input_buffer()
        self.ser.write((cmd + '\r').encode())

        lines = []
        deadline = time.time() + self.timeout

        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue

            line = raw.decode(errors='replace').strip()

            if not line:
                continue
            if line == cmd:          # 跳過 echo
                continue
            if line == 'OK':
                return True, 0, lines
            if line.startswith('ERR'):
                parts = line.split()
                code = int(parts[1]) if len(parts) > 1 else -1
                return False, code, lines

            if verbose:
                print('  ' + line)
            lines.append(line)

        raise TimeoutError(f'no OK/ERR for command: {cmd}')

def cmd_write(fl, args):
    for i in range(args.count):
        text = args.text if args.count == 1 else f'{args.text}-{i}'
        ok, code, _ = fl.send(f'log write {text}')
        if not ok:
            print(f'write #{i} failed: ERR {code}')
            return 1
    print(f'wrote {args.count} record(s)')
    return 0


def cmd_dump(fl, args):
    cmd = f'log dump {args.start} {args.count}'
    ok, code, lines = fl.send(cmd)
    for l in lines:
        print(l)
    return 0 if ok else 1


def cmd_stats(fl, args):
    ok, code, lines = fl.send('log stats')
    for l in lines:
        print(l)
    return 0 if ok else 1


def cmd_format(fl, args):
    ok, code, _ = fl.send('log format')
    print('format OK' if ok else f'format failed: ERR {code}')
    return 0 if ok else 1


def cmd_benchmark(fl, args):
    """量測 log append 的延遲分布"""
    payload = 'x' * args.size
    results = []

    print(f'benchmark: {args.count} appends of {args.size} bytes')

    if args.format:
        fl.send('log format')

    for i in range(args.count):
        t0 = time.perf_counter()
        ok, code, _ = fl.send(f'log write {payload}')
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        if not ok:
            print(f'  stopped at #{i}: ERR {code}')
            break

        results.append(elapsed_ms)

        if (i + 1) % 50 == 0:
            print(f'  {i + 1}/{args.count}')

    if not results:
        print('no data')
        return 1

    results_sorted = sorted(results)
    n = len(results_sorted)

    def pct(p):
        idx = min(int(n * p / 100), n - 1)
        return results_sorted[idx]

    print(f'\n  count : {n}')
    print(f'  min   : {results_sorted[0]:.2f} ms')
    print(f'  p50   : {pct(50):.2f} ms')
    print(f'  p95   : {pct(95):.2f} ms')
    print(f'  p99   : {pct(99):.2f} ms')
    print(f'  max   : {results_sorted[-1]:.2f} ms')
    print(f'  mean  : {sum(results) / n:.2f} ms')

    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['index', 'latency_ms'])
            for i, v in enumerate(results):
                w.writerow([i, f'{v:.3f}'])
        print(f'\n  saved to {args.csv}')

    return 0

def _get_stat(fl, key):
    """從 log stats 取出一個數值欄位"""
    _, _, lines = fl.send('log stats')
    for line in lines:
        for tok in line.split():
            if tok.startswith(key + '='):
                return int(tok.split('=')[1])
    return None


def cmd_scaling(fl, args):
    """量測 mount 時間隨 record 筆數的變化"""
    payload = 'x' * args.size
    results = []

    print(f'scaling: up to {args.max} records, step {args.step}, '
          f'payload {args.size} B')
    fl.send('log format')

    total = 0
    while total < args.max:
        # 補到下一個測量點
        for _ in range(args.step):
            ok, code, _ = fl.send(f'log write {payload}')
            if not ok:
                print(f'  write failed at {total}: ERR {code}')
                args.max = total
                break
            total += 1

        # 重新掛載並取得掃描時間
        fl.send('log remount')
        init_us = _get_stat(fl, 'init_us')
        used    = _get_stat(fl, 'used')

        results.append((total, init_us, used))
        print(f'  {total:5d} records  {init_us/1000:7.2f} ms  '
              f'({init_us/total:6.1f} us/record)')

    if not results:
        return 1

    # 線性度檢查
    first, last = results[0], results[-1]
    ratio_n  = last[0] / first[0]
    ratio_us = last[1] / first[1]
    print(f'\n  records x{ratio_n:.1f}  ->  mount time x{ratio_us:.1f}')
    print(f'  {"linear (O(n)) confirmed" if abs(ratio_us - ratio_n) / ratio_n < 0.2 else "non-linear"}')

    if args.csv:
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['records', 'init_us', 'used_bytes', 'us_per_record'])
            for n, us, used in results:
                w.writerow([n, us, used, f'{us/n:.2f}'])
        print(f'\n  saved to {args.csv}')

    return 0

def main():
    p = argparse.ArgumentParser(description='FlashLog host CLI')
    p.add_argument('--port', default='COM3')
    p.add_argument('--baud', type=int, default=115200)
    sub = p.add_subparsers(dest='cmd', required=True)

    sp = sub.add_parser('write', help='append record(s)')
    sp.add_argument('text')
    sp.add_argument('--count', type=int, default=1)
    sp.set_defaults(func=cmd_write)

    sp = sub.add_parser('dump', help='list records')
    sp.add_argument('--start', type=int, default=0, help='start rec_id (0 = last N)')
    sp.add_argument('--count', type=int, default=20)
    sp.set_defaults(func=cmd_dump)

    sp = sub.add_parser('stats', help='show log stats')
    sp.set_defaults(func=cmd_stats)

    sp = sub.add_parser('format', help='erase log area')
    sp.set_defaults(func=cmd_format)

    sp = sub.add_parser('benchmark', help='measure append latency')
    sp.add_argument('--count', type=int, default=100)
    sp.add_argument('--size', type=int, default=16)
    sp.add_argument('--format', action='store_true', help='format before test')
    sp.add_argument('--csv', help='write results to CSV file')
    sp.set_defaults(func=cmd_benchmark)

    sp = sub.add_parser('scaling', help='measure mount time vs record count')
    sp.add_argument('--max', type=int, default=200)
    sp.add_argument('--step', type=int, default=25)
    sp.add_argument('--size', type=int, default=16)
    sp.add_argument('--csv')
    sp.set_defaults(func=cmd_scaling)

    args = p.parse_args()

    fl = FlashLog(args.port, args.baud)
    try:
        return args.func(fl, args)
    finally:
        fl.close()


if __name__ == '__main__':
    sys.exit(main())