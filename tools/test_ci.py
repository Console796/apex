"""Small orchestration regressions; does not build or launch a CI run."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import ci
import ci_top


class Report:
    def __init__(self, directory):
        self.dir = Path(directory)
        self.rows = []

    def add(self, *row):
        self.rows.append(row)


class ParallelCiTests(unittest.TestCase):
    def test_core_workers_and_remaining_ctest_entries_are_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Report(directory)
            with patch.object(ci.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as run, \
                    patch.dict(os.environ, {'APEX_TEST_JOBS': '99'}):
                self.assertTrue(ci.test_stage('linux-asan', 3, report))
            self.assertEqual(run.call_count, 2)
            core, rest = run.call_args_list
            self.assertEqual(core.args[0][-1], '--jobs=3')
            self.assertTrue(core.args[0][0].endswith('/tests/apex_tests'))
            self.assertEqual(rest.args[0], ['ctest', '--preset', 'linux-asan', '-LE', '^apex-tests$', '-j', '3'])
            self.assertNotIn('APEX_TEST_JOBS', core.kwargs['env'])
            self.assertIn('tests/lsan.supp', core.kwargs['env']['LSAN_OPTIONS'])
            self.assertEqual(core.kwargs['env']['APEX_TEST_PROGRESS_DIR'], str(Path(directory) / 'linux-asan/test-progress'))
            self.assertEqual(report.rows[0][1], 'ok')

    def test_failure_in_either_part_fails_the_stage_without_skipping_other_coverage(self):
        for codes in [(1, 0), (0, 1)]:
            with self.subTest(codes=codes), tempfile.TemporaryDirectory() as directory:
                report = Report(directory)
                outcomes = [subprocess.CompletedProcess([], code) for code in codes]
                with patch.object(ci.subprocess, 'run', side_effect=outcomes) as run:
                    self.assertFalse(ci.test_stage('linux-clang-release', 2, report))
                self.assertEqual(run.call_count, 2)
                self.assertEqual(report.rows[0][1], 'FAIL')

    def test_monitor_reads_slice_progress(self):
        fraction, detail = ci_top.log_progress('linux-asan:test', [
            '[apex_tests] slice 3/16 (cases 11-15) ok in 0.2s  [4/16 done]'])
        self.assertEqual(fraction, .25)
        self.assertEqual(detail, 'apex_tests slices 4/16')


if __name__ == '__main__':
    unittest.main()
