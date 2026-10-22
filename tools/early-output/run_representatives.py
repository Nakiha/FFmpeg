#!/usr/bin/env python3
"""Active replacement for the frozen phase3 runner. Fresh outputs only."""
import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid
from evidence import EvidenceError, Expectation, compare, require, sha256, validate


def existing_input(workspace, folder, filename):
    file = workspace / 'out/phase2' / folder / filename
    manifest = json.loads((file.parent / 'manifest.json').read_text())
    row = next(r for r in manifest if Path(r['file']).name == filename)
    require(sha256(file) == row['sha256'], 'fixture no longer matches frozen manifest')
    command = row['command']
    frames = int(command[command.index('-frames:v') + 1])
    require(frames > 0, 'missing independent fixture frame count')
    return file, frames


def build_plan(workspace, full):
    inputs = [('hrd-targets', 'x264-' + n + '.mp4') for n in
              ['fixed3-pyr1-open0', 'fixed7-pyr1-open1', 'bdominant16-pyr1-open1', 'adaptive16-pyr1-open1']]
    inputs += [('targets', c + '-fixed7-pyr1-open1.mp4') for c in ['x265', 'x264']]
    cases = []
    for folder, filename in inputs:
        file, frames = existing_input(workspace, folder, filename)
        for mode, threads in [('legacy', 1), ('legacy', 2), ('legacy', 12), ('async', 2), ('async', 12)]:
            for early in [0, 1]:
                cases.append(dict(id=f'{folder}-{file.stem}-{mode}-t{threads}-early{early}', file=file, frames=frames,
                                  mode=mode, threads=threads, early=early, ref_build='baseline', ref_mode='legacy', ref_cycles=1, cycles=3))
    if full:
        file, frames = existing_input(workspace, 'targets', 'x264-fixed3-pyr1-open0.mp4')
        certificate = workspace / 'out/phase2/target-proof-final/x264-fixed3-pyr1-open0.packet-sha256'
        require(certificate.is_file(), 'offline certificate missing')
        for mode, threads in [('legacy', 1), ('async', 2), ('async', 12)]:
            cases.append(dict(id=f'certificate-{mode}-t{threads}', file=file, frames=frames, mode=mode, threads=threads, early=1,
                              ref_build='baseline', ref_mode='legacy', ref_cycles=1, cycles=3, certificate=certificate))
        oracle = json.loads((workspace / 'out/source-audit/summary.json').read_text())
        pcm = json.loads((workspace / 'out/samples/pcm-manifest.json').read_text())
        for filename in ['camp_mot_mbaff0_full.26l', 'Sharp_MP_PAFF_1r2.jvt', 'extradata-reload-multi-stsd.mov',
                         'pcm-poc0-leading-negative.h264', 'pcm-poc2-invalid-leading.h264']:
            file = workspace / 'out/samples' / filename
            truth = next((r for r in oracle if r['file'] == filename), None)
            if truth:
                digest, frames = truth['sha256'], truth['independent_frames']
            else:
                truth = next(r for r in pcm if Path(r['path']).name == filename)
                digest, frames = truth['sha256'], len(truth['frames'])
            require(sha256(file) == digest, 'fallback fixture identity mismatch')
            for early in [0, 1]:
                cases.append(dict(id=f'fallback-{file.stem}-early{early}', file=file, frames=frames, mode='async', threads=2, early=early,
                                  ref_build='baseline', ref_mode='async', ref_cycles=1, cycles=1))
        for codec, threads in [('x264', 12), ('x265', 2)]:
            file, frames = existing_input(workspace, 'hrd-targets' if codec == 'x264' else 'targets', codec + '-adaptive16-pyr1-open1.mp4')
            for mode in ['gate', 'flush', 'error', 'close', 'api']:
                cases.append(dict(id=f'lifecycle-{codec}-{mode}', file=file, frames=frames, mode=mode, threads=threads, early=1,
                                  ref_build='candidate', ref_mode=mode, ref_cycles=3, cycles=3))
    return cases


def verify_identity(build):
    require(sha256(build['binary']) == build['binary_sha256'], 'probe binary changed')
    require(all(sha256(p) == digest for p, digest in build['libraries'].items()), 'SDK library changed')


def run(build, case, prefix, mode, cycles, early, certificate=None):
    verify_identity(build)
    env = dict(os.environ)
    for key in ['EARLY_OUTPUT', 'H264_CERTIFICATE', 'DYLD_INSERT_LIBRARIES', 'DYLD_LIBRARY_PATH', 'LD_LIBRARY_PATH',
                'SYNTHETIC_PTS', 'STRICT_ERRORS', 'NO_HASH', 'SEEK_US', 'TEST_FIRST_TWO_REORDER',
                'FFMPEG_LIVE_IDR_EARLY', 'FFMPEG_LIVE_POC_EARLY', 'AU_MUTATION', 'MUTATION_AU', 'FAIL_FIFO_WRITE']:
        env.pop(key, None)
    # The baseline does not have the new H264 option; do not request it there.
    if early is not None:
        env['EARLY_OUTPUT'] = str(early)
    if certificate:
        env['H264_CERTIFICATE'] = str(certificate)
    command = [build['binary'], str(case['file']), str(case['threads']), '0', mode, str(prefix), str(cycles), '0', '1']
    with prefix.with_suffix('.log').open('x') as log:
        try:
            ret = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=40).returncode
        except subprocess.TimeoutExpired:
            ret = 'timeout'
    verify_identity(build)
    text = prefix.with_suffix('.log').read_text()
    if certificate:
        require(re.findall(r'^certificate_verified=(\d+)', text, re.M) == ['1'], 'offline contract was not actually verified')
    loaded = re.findall(r'^runtime_avcodec=(.+)$', text, re.M)
    require(len(loaded) == 1, 'runtime library identity missing')
    require(str(Path(loaded[0]).resolve()) in build['libraries'], 'unexpected runtime libavcodec')
    expectation = Expectation(cycles, case['frames'], 'close' if mode == 'close' else 'decode')
    frames = validate(prefix, ret, expectation)
    record = dict(command=command, returncode=ret, expected=asdict(expectation), prefix=str(prefix),
                  binary_sha256=build['binary_sha256'], runtime_libavcodec_sha256=sha256(loaded[0]),
                  files={suffix: sha256(prefix.with_suffix(suffix)) for suffix in ['.csv', '.log', '.sha256']})
    return frames, expectation, record


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workspace', type=Path, required=True)
    ap.add_argument('--identity', type=Path, required=True, help='build_probes.py identity.json')
    ap.add_argument('--results', type=Path, required=True, help='Must not already exist')
    ap.add_argument('--full', action='store_true', help='83 cases including fallbacks/certificate/lifecycle; otherwise 60 core cases')
    ap.add_argument('--filter', default='', help='Explicit subset by case-id substring; recorded in plan')
    args = ap.parse_args()
    plan = build_plan(args.workspace.resolve(), args.full)
    cases = [c for c in plan if args.filter in c['id']]
    require(cases and len({c['id'] for c in cases}) == len(cases), 'empty/duplicate case selection')
    identity = json.loads(args.identity.read_text())
    root = args.results.resolve()
    root.mkdir(parents=True, exist_ok=False)
    doc = dict(run_id=str(uuid.uuid4()), started_utc=datetime.now(timezone.utc).isoformat(), status='running',
               identity=identity, filter=args.filter, full_plan_count=len(plan), selected_count=len(cases),
               cases=[{**c, 'file': str(c['file']), 'input_sha256': sha256(c['file']), **({'certificate': str(c['certificate']), 'certificate_sha256': sha256(c['certificate'])} if c.get('certificate') else {})} for c in cases])
    (root / 'plan.json').write_text(json.dumps(doc, indent=2) + '\n')
    results = []
    for case in cases:
        directory = root / case['id']
        directory.mkdir()
        row = {'id': case['id'], 'passed': False}
        try:
            initial_input = sha256(case['file'])
            a, ea, ra = run(identity['builds'][case['ref_build']], case, directory / 'reference', case['ref_mode'], case['ref_cycles'],
                            0 if case['ref_build'] == 'candidate' else None)
            row['reference'] = ra
            b, eb, rb = run(identity['builds']['candidate'], case, directory / 'result', case['mode'], case['cycles'], case['early'], case.get('certificate'))
            row['result'] = rb
            require(sha256(case['file']) == initial_input, 'input changed while running')
            row['comparison'] = compare(a, b, ea, eb, allow_dts=bool(case['early']))
            row['passed'] = True
        except (EvidenceError, OSError, ValueError, KeyError) as error:
            row['error'] = str(error)
        results.append(row)
        (root / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
        print(case['id'], 'PASS' if row['passed'] else 'FAIL ' + row.get('error', ''), flush=True)
    doc.update(status='passed' if all(r['passed'] for r in results) else 'failed', completed_count=len(results))
    (root / 'plan.json').write_text(json.dumps(doc, indent=2) + '\n')
    return 0 if doc['status'] == 'passed' else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (EvidenceError, OSError, ValueError, KeyError, StopIteration) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
