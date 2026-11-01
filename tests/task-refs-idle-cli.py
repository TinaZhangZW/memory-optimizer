#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""CLI failures must never replace an existing snapshot or start migration."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


class IdleCommandLine(unittest.TestCase):
    def test_reject_invalid_arguments_without_replacing_output(self):
        range_error = (
            'idle-bitmap requires positive PID, aligned range and output file'
        )
        legacy_error = (
            'idle-bitmap uses --window-ms; '
            'legacy scan/migration options do not apply'
        )
        option_error = 'unrecognized or incomplete option; see --help'
        cases = [
            (['--window-ms', '0'], '--window-ms is out of range'),
            (['--window-ms', '-1'], 'invalid unsigned number: -1'),
            (['--window-ms', '4294967296'], '--window-ms is out of range'),
            (['--ram-range', '0x1001:0x2000'], range_error),
            (['--ram-range', '0x2000:0x1000'], range_error),
            (['--ram-range', '010000:020000'], range_error),
            (['--ram-range', '0x1000'], '--ram-range requires START:END'),
            (['--ram-range', '0x1000:0x2000:0x3000'],
             'invalid unsigned number: 0x2000:0x3000'),
            (['--ram-range', ':8192'], 'invalid unsigned number:'),
            (['--ram-range', '4096:'], 'invalid unsigned number:'),
            (['--ram-range', '0x:8192'], 'invalid unsigned number: 0x'),
            (['--pid', '-1'], 'invalid unsigned number: -1'),
            (['--pid', '0'], 'PID is out of range'),
            (['--pid', '1junk'], 'invalid unsigned number: 1junk'),
            (['--pid', '4294967297'], 'PID is out of range'),
            (['--pid', '18446744073709551616'],
             'invalid unsigned number: 18446744073709551616'),
            (['--scan', 'unknown'], 'supported --scan value: idle-bitmap'),
            (['--migrate', 'cold'], legacy_error),
            (['--interval', '0.1'], legacy_error),
            (['--loop', '2'], legacy_error),
            (['--unknown-option'], option_error),
            (['--window-ms'], option_error),
            (['extra'], 'unexpected positional argument'),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'pages.tsv'
            output.write_text('old snapshot\n')
            base = [
                str(ROOT / 'task-refs'),
                '--scan', 'idle-bitmap',
                '--pid', '2147483647',
                '--ram-range', '0x1000:0x2000',
                '--output', str(output),
            ]

            for extra, expected_error in cases:
                with self.subTest(extra=extra):
                    result = subprocess.run(
                        base + extra,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=5,
                        universal_newlines=True,
                    )

                    self.assertEqual(result.returncode, 1)
                    self.assertIn('task-refs: ' + expected_error, result.stderr)
                    self.assertNotIn('pidfd_open', result.stderr)
                    self.assertEqual(output.read_text(), 'old snapshot\n')
                    self.assertEqual(
                        list(pathlib.Path(tmp).iterdir()), [output]
                    )

    def test_valid_address_formats_reach_target_lookup_without_publishing(self):
        # Each range denotes [4096, 8192), including zero-padded decimal.
        ranges = [
            '4096:8192',
            '00004096:00008192',
            '0x1000:0x2000',
            '0X1000:0X2000',
            '00004096:0X2000',
            '0x1000:00008192',
        ]
        with tempfile.TemporaryDirectory() as tmp:
            output = pathlib.Path(tmp) / 'pages.tsv'

            for address_range in ranges:
                with self.subTest(address_range=address_range):
                    # Linux PID allocation is bounded far below INT_MAX.
                    result = subprocess.run(
                        [
                            str(ROOT / 'task-refs'),
                            '--scan', 'idle-bitmap',
                            '--pid', '2147483647',
                            '--ram-range', address_range,
                            '--output', str(output),
                        ],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=5,
                        universal_newlines=True,
                    )

                    self.assertEqual(result.returncode, 1)
                    self.assertIn('task-refs: pidfd_open:', result.stderr)
                    self.assertEqual(list(pathlib.Path(tmp).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
