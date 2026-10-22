#!/usr/bin/env python3
"""Link instrumented probes against explicitly selected, existing SDKs."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
from evidence import sha256


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--baseline-prefix', type=Path, required=True)
    ap.add_argument('--candidate-prefix', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True, help='New directory; never reused')
    ap.add_argument('--sanitize', default='', choices=['', 'address,undefined'], help='Instrument/link the probes for a matching sanitizer SDK')
    ap.add_argument('--probe', choices=['probe.c', 'load.c'], default='probe.c')
    args = ap.parse_args()
    source = Path(__file__).with_name(args.probe).resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    identities = {}
    for name, prefix in [('baseline', args.baseline_prefix), ('candidate', args.candidate_prefix)]:
        prefix = prefix.resolve()
        env = dict(os.environ, PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'))
        env.pop('PKG_CONFIG_PATH', None)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libavformat', 'libavcodec', 'libavutil'], env=env, text=True))
        binary = (args.output / name).resolve()
        command = [os.environ.get('CC', 'cc'), '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', str(source), *flags,
                   '-Wl,-rpath,' + str(prefix / 'lib'), '-pthread', '-o', str(binary)]
        if args.probe == 'load.c':
            command.append('-DLIVE_ASYNC')
        if args.sanitize:
            command += ['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer']
        if platform.system() == 'Linux':
            command.append('-ldl')
        subprocess.run(command, check=True, env=env)
        libraries = {str(p.resolve()): sha256(p) for p in sorted((prefix / 'lib').glob('libav*')) if p.is_file() and ('.so' in p.name or p.suffix == '.dylib')}
        if not any('libavcodec' in p for p in libraries):
            raise RuntimeError('No shared libavcodec in selected SDK')
        identities[name] = {'binary': str(binary), 'binary_sha256': sha256(binary), 'libraries': libraries, 'compile_command': command}
    record = {'probe_source_sha256': sha256(source), 'builds': identities}
    (args.output / 'identity.json').write_text(json.dumps(record, indent=2) + '\n')
    print(args.output / 'identity.json')


if __name__ == '__main__':
    main()
