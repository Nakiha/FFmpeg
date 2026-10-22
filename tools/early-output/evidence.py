"""Fail-closed checks for the instrumented early-output probe. No codec calls."""
from dataclasses import dataclass
import csv
import hashlib
from pathlib import Path
import re


class EvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


@dataclass(frozen=True)
class Expectation:
    cycles: int
    frames_per_cycle: int
    mode: str = 'decode'

    def __post_init__(self):
        require(self.mode in ('decode', 'close'), 'unknown expectation mode')
        require(self.cycles > 0 and self.frames_per_cycle > 0, 'positive declared counts required')


def validate(prefix, returncode, expected):
    """Counts come ONLY from the plan, not from the observed output length."""
    prefix = Path(prefix)
    require(returncode == 0, f'process did not succeed: {returncode}')
    log = prefix.with_suffix('.log').read_text()
    require(re.findall(r'^terminal_return=(-?\d+)$', log, re.M) == ['0'], 'missing/failed/duplicate terminal status')
    rows = list(csv.reader(prefix.with_suffix('.sha256').read_text().splitlines()))
    events = list(csv.reader(prefix.with_suffix('.csv').read_text().splitlines()))
    if expected.mode == 'close':
        require(not rows, 'close test unexpectedly published frames')
        require(re.findall(r'^close_inflight_verified=1$', log, re.M) == ['close_inflight_verified=1'], 'close did not verify an in-flight worker')
        require(events == [['cycle_begin', '0']], 'close must stop during the first cycle')
        return rows
    require(len(rows) == expected.cycles * expected.frames_per_cycle, 'wrong declared total frame count')
    require(all(len(row) == 19 and re.fullmatch('[0-9a-f]{64}', row[13]) for row in rows), 'malformed frame digest')
    cursor = 0
    for cycle in range(expected.cycles):
        require(cursor < len(events) and events[cursor] == ['cycle_begin', str(cycle)], 'missing/reordered cycle begin')
        cursor += 1
        for frame in range(expected.frames_per_cycle):
            require(cursor < len(events), 'missing frame event')
            event = events[cursor]
            require(len(event) == 5 and event[:3] == ['frame', str(cycle), str(frame)], 'missing/duplicate/reordered frame event')
            int(event[3]); int(event[4])
            require(event[3] == rows[cycle * expected.frames_per_cycle + frame][18], 'event/digest source identity mismatch')
            cursor += 1
        require(cursor < len(events) and events[cursor] == ['cycle_end', str(cycle), str(expected.frames_per_cycle)], 'missing/incorrect cycle completion')
        cursor += 1
    require(cursor + 1 == len(events) and events[cursor][0] == 'summary', 'missing/extra summary or events')
    fields = dict(item.split('=', 1) for item in events[cursor][1:])
    require(fields.get('frames') == str(expected.cycles * expected.frames_per_cycle), 'summary count mismatch')
    return rows


def compare(reference, result, reference_expected, result_expected, allow_dts=False):
    require(reference_expected.mode == result_expected.mode, 'different comparison contracts')
    if result_expected.mode == 'close':
        require(not reference and not result, 'invalid close evidence')
        return {'kind': 'inflight-close-only', 'pixel_comparison': False}
    require(reference_expected.frames_per_cycle == result_expected.frames_per_cycle, 'different per-cycle ground truth')
    n = reference_expected.frames_per_cycle
    require(len(reference) == n * reference_expected.cycles and len(result) == n * result_expected.cycles, 'unvalidated count')
    base = reference[:n]
    # Prove every declared reference cycle too; never infer a repetition count.
    require(reference == base * reference_expected.cycles, 'reference replay changed')
    want = base * result_expected.cycles
    columns = sorted({i for a, b in zip(want, result) for i, (u, v) in enumerate(zip(a, b)) if u != v})
    require(not columns or (allow_dts and columns == [2]), f'ordered frame metadata differs in columns {columns}')
    return {'kind': 'ordered-frames', 'pixel_comparison': True, 'expected_frames': len(want), 'differing_columns': columns}
