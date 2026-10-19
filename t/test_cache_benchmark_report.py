"""Report interrupted measurements without promoting them to complete runs."""
import csv
import json
from pathlib import Path
import tempfile
import unittest

from cache_benchmark_report import MODES, write_report


class BenchmarkReport(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="benchmark report ")
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name)
        self.save(self.output / "environment.json", dict(revision="fixture", cpu_tick_hz=100,
            options=dict(mode="compare", trials=10, files=1000, variants=2, workers=1, levels="1:2")))

    @staticmethod
    def save(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value))

    def mode(self, mode, count, controls=False):
        trials = []
        for trial in range(1, count + 1):
            row = dict(mode=mode, trial=trial, files=1002, variants=2,
                ack_ms=float(trial), files_gone_observed_ms=20.0, drained_observed_ms=40.0,
                resources_before={"42": dict(Pss=100, fds=10, cpu_ticks=1)},
                resources_after={"42": dict(Pss=104, fds=10, cpu_ticks=3)},
                probes={phase: {kind: dict(n=0, max_ms=None) for kind in ("ping", "hit")}
                        for phase in ("baseline", "ack", "cleanup")})
            if controls:
                row["client_control_ms"] = [.1, .2, .3]
            trials.append(row)
        self.save(self.output / mode / "results.json", dict(setup_seconds=60, trials=trials))

    def test_timeout_keeps_verified_trials_and_missing_mode(self):
        self.mode("legacy-vary", 10)
        self.mode("generation", 9)
        self.save(self.output / "generation/progress.json", dict(phase="validate-files", trial=10))
        summary, complete = write_report(self.output, "failure")
        self.assertFalse(complete)
        self.assertIn("**INCOMPLETE**", summary)
        self.assertIn("Verified trials: 19/30", summary)
        self.assertIn("| generation-threads | 0/10 | n/a |", summary)
        self.assertIn("validate-files (trial 10)", summary)
        with (self.output / "purges.csv").open() as stream:
            self.assertEqual(len(list(csv.DictReader(stream))), 19)

    def test_count_alone_does_not_establish_success(self):
        for mode in MODES:
            self.mode(mode, 10)
        self.assertFalse(write_report(self.output, "failure")[1])
        self.assertFalse(write_report(self.output, "unknown")[1])
        self.assertTrue(write_report(self.output, "success")[1])
        self.save(self.output / "generation/failure.json", dict(error="shutdown failed"))
        self.assertFalse(write_report(self.output, "success")[1])

    def test_truncated_mode_does_not_discard_other_modes(self):
        self.mode("legacy-vary", 10)
        root = self.output / "generation"
        root.mkdir()
        (root / "results.json").write_text('{"trials":[')
        summary, complete = write_report(self.output, "failure")
        self.assertFalse(complete)
        self.assertIn("Verified trials: 10/30", summary)
        self.assertIn("Cannot read generation/results.json", summary)

    def test_out_of_order_trials_are_not_verified(self):
        self.mode("legacy-vary", 2)
        path = self.output / "legacy-vary/results.json"
        data = json.loads(path.read_text())
        data["trials"][1]["trial"] = 1
        self.save(path, data)
        summary, complete = write_report(self.output, "success")
        self.assertFalse(complete)
        self.assertIn("Verified trials: 0/30", summary)
        self.assertIn("unexpected mode or trial order", summary)

    def test_control_does_not_get_subtracted_from_ack(self):
        self.mode("legacy-vary", 10, controls=True)
        summary, _ = write_report(self.output, "failure")
        self.assertIn("| legacy-vary | 10/10 | 0.200 | 5.500 [1.000, 10.000] |", summary)
        self.assertIn("| legacy-vary | 1.000 | 6.000 | 9 |", summary)
        self.assertIn("| legacy-vary | 20.000 | 4.000 | 0 |", summary)

    def test_missing_environment_is_incomplete(self):
        (self.output / "environment.json").unlink()
        summary, complete = write_report(self.output, "failure")
        self.assertFalse(complete)
        self.assertIn("**INCOMPLETE**", summary)
        self.assertTrue((self.output / "summary.md").exists())

    def test_report_replaces_read_only_container_outputs(self):
        self.mode("legacy-vary", 10)
        write_report(self.output, "failure")
        for name in ("summary.md", "purges.csv"):
            (self.output / name).chmod(0o444)
        self.mode("generation", 9)
        summary, complete = write_report(self.output, "failure")
        self.assertFalse(complete)
        self.assertIn("Verified trials: 19/30", summary)
        self.assertEqual((self.output / "summary.md").read_text(), summary)


if __name__ == "__main__":
    unittest.main()
