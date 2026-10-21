#!/usr/bin/env python3
"""Compare opt-in IDR output with unchanged decoder, preserving all but pkt_dts.

pkt_dts denotes the packet that triggered output, so early return changes it.
Every other emitted pixel/metadata field, including presentation order, is exact.
Creates a fresh evidence directory and never rewrites a frozen golden.
"""
import argparse
import json
import os
import pathlib
import subprocess


def check(probe, inputs, output):
    output.mkdir(parents=True, exist_ok=False)
    base = dict(os.environ)
    base.pop('FFMPEG_LIVE_IDR_EARLY', None)
    base.pop('FFMPEG_LIVE_AUTO_MIN', None)
    enabled = dict(base, FFMPEG_LIVE_IDR_EARLY='1', FFMPEG_LIVE_AUTO_MIN='2')
    results = []
    def run(source, threads, mode, stem, env, cycles=1, capacity=1):
        prefix = output/stem
        command = [str(probe), str(source), str(threads), '0', mode, str(prefix),
                   str(cycles), '0', str(capacity)]
        with prefix.with_suffix('.log').open('x') as log:
            subprocess.run(command, env=env, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        return [line.split(',') for line in prefix.with_suffix('.sha256').read_text().splitlines()]
    for index, source in enumerate(inputs):
        for threads in (2, 12):
            stem = f'{index}-{source.stem}-t{threads}'
            expected = run(source, threads, 'legacy', stem+'-off', base)
            assert expected, source
            for cycles in (1, 3):
                got = run(source, threads, 'async', stem+f'-on-{cycles}', enabled, cycles)
                reference = expected*cycles
                assert len(got) == len(reference), (source, len(got), len(reference))
                changed = []
                for number, (a, b) in enumerate(zip(reference, got)):
                    assert a[:2]+a[3:] == b[:2]+b[3:], (source, number, a, b)
                    if a[2] != b[2]:
                        changed.append(dict(frame=number, pts=a[0], old_pkt_dts=a[2], new_pkt_dts=b[2]))
                results.append(dict(input=str(source), threads=threads, cycles=cycles,
                                    frames=len(got), exact_pixels_pts_metadata=True,
                                    trigger_pkt_dts_changes=changed))
    (output/'summary.json').write_text(json.dumps(dict(status='passed', comparisons=results), indent=2)+'\n')
    print('PASS IDR early output:', output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    p.add_argument('inputs', type=pathlib.Path, nargs='+')
    args = p.parse_args()
    check(args.probe.resolve(), [path.resolve() for path in args.inputs], args.output.resolve())
