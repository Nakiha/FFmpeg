import json
from pathlib import Path
import tempfile
import unittest
from evidence import EvidenceError, Expectation, compare, validate


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name) / 'case'
        self.expected = Expectation(3, 2)

    def write(self, cycles=3, count=2, terminal='0', close=False):
        digest, events = [], []
        for c in range(cycles):
            events.append(f'cycle_begin,{c}')
            for i in range(count):
                row = ['0'] * 19
                row[0] = str(i); row[13] = ('a' if i == 0 else 'b') * 64; row[18] = str(i)
                digest.append(','.join(row)); events.append(f'frame,{c},{i},{i},10')
            events.append(f'cycle_end,{c},{count}')
        events.append(f'summary,frames={cycles*count}')
        if close:
            digest, events = [], ['cycle_begin,0']
        self.prefix.with_suffix('.sha256').write_text('\n'.join(digest) + ('\n' if digest else ''))
        self.prefix.with_suffix('.csv').write_text('\n'.join(events) + '\n')
        self.prefix.with_suffix('.log').write_text(('close_inflight_verified=1\n' if close else '') + f'terminal_return={terminal}\n')

    def test_complete_declared_cycles(self):
        self.write(); result = validate(self.prefix, 0, self.expected)
        self.assertEqual(compare(result[:2], result, Expectation(1, 2), self.expected)['expected_frames'], 6)

    def test_zero_frames_rejected(self):
        self.write(cycles=0)
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_whole_missing_cycles_rejected(self):
        self.write(cycles=1)
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_both_empty_rejected(self):
        with self.assertRaises(EvidenceError): compare([], [], Expectation(1, 2), self.expected)

    def test_nonzero_process_with_complete_output_rejected(self):
        self.write()
        with self.assertRaises(EvidenceError): validate(self.prefix, 1, self.expected)

    def test_timeout_with_stale_complete_output_rejected(self):
        self.write()
        with self.assertRaises(EvidenceError): validate(self.prefix, 'timeout', self.expected)

    def test_terminal_error_rejected(self):
        self.write(terminal='-1')
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_missing_terminal_rejected(self):
        self.write(); self.prefix.with_suffix('.log').write_text('')
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_duplicate_terminal_rejected(self):
        self.write(); self.prefix.with_suffix('.log').write_text('terminal_return=0\nterminal_return=0\n')
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_missing_cycle_completion_rejected(self):
        self.write(); p = self.prefix.with_suffix('.csv'); p.write_text(p.read_text().replace('cycle_end,2,2\n', ''))
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_duplicate_frame_event_rejected(self):
        self.write(); p = self.prefix.with_suffix('.csv'); p.write_text(p.read_text().replace('frame,1,1,1,10', 'frame,1,0,1,10'))
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_source_mapping_mismatch_rejected(self):
        self.write(); p = self.prefix.with_suffix('.csv'); p.write_text(p.read_text().replace('frame,0,1,1,10', 'frame,0,1,0,10'))
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_wrong_summary_rejected(self):
        self.write(); p = self.prefix.with_suffix('.csv'); p.write_text(p.read_text().replace('frames=6', 'frames=2'))
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_order_change_rejected(self):
        self.write(); result = validate(self.prefix, 0, self.expected); result[0], result[1] = result[1], result[0]
        with self.assertRaises(EvidenceError): compare(result[2:4], result, Expectation(1, 2), self.expected)

    def test_dts_exception_is_narrow(self):
        self.write(); result = validate(self.prefix, 0, self.expected); base = [x.copy() for x in result[:2]]; result[0][2] = '17'
        compare(base, result, Expectation(1, 2), self.expected, allow_dts=True)
        with self.assertRaises(EvidenceError): compare(base, result, Expectation(1, 2), self.expected)
        result[0][0] = '99'
        with self.assertRaises(EvidenceError): compare(base, result, Expectation(1, 2), self.expected, allow_dts=True)

    def test_close_requires_actual_inflight_checkpoint(self):
        self.write(close=True); expected = Expectation(3, 2, 'close')
        self.assertEqual(validate(self.prefix, 0, expected), [])
        self.prefix.with_suffix('.log').write_text('terminal_return=0\n')
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, expected)

    def test_close_cannot_pass_as_decode(self):
        self.write(close=True)
        with self.assertRaises(EvidenceError): validate(self.prefix, 0, self.expected)

    def test_invalid_expected_count_rejected(self):
        with self.assertRaises(EvidenceError): Expectation(0, 192)
        with self.assertRaises(EvidenceError): Expectation(3, 0)


if __name__ == '__main__':
    unittest.main()
