#!/usr/bin/env python3
"""Summarize verified trials, including artifacts from an interrupted experiment."""
import argparse
import csv
import io
import json
import math
from pathlib import Path
from statistics import median
import tempfile

MODES = ("legacy-vary", "generation", "generation-threads")
FIELDS = ("mode", "trial", "files", "variants", "ack_ms",
          "files_gone_observed_ms", "drained_observed_ms")


def replace_text(path, contents):
    # Successful container runs leave root-owned outputs. The runner owns the
    # parent directory and can replace them, but cannot truncate them in place.
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False,
                                     prefix=".benchmark-", newline="") as stream:
        temporary = Path(stream.name)
        try:
            stream.write(contents)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path, issues):
    try:
        value = json.loads(path.read_text())
        if not isinstance(value, dict):
            raise ValueError("expected an object")
        return value
    except (OSError, ValueError) as error:
        issues.append(f"Cannot read {path.parent.name}/{path.name}: {error}")
        return {}


def milliseconds(values, spread=False):
    if not values:
        return "n/a"
    result = f"{median(values):.3f}"
    if spread:
        result += f" [{min(values):.3f}, {max(values):.3f}]"
    return result


def probe_cell(rows, phase, kind):
    samples = [r["probes"][phase][kind] for r in rows]
    count = sum(s["n"] for s in samples)
    maximum = max((s["max_ms"] for s in samples if s["max_ms"] is not None), default=None)
    return f"{count} / {maximum:.3f}" if maximum is not None else f"{count} / n/a"


def resource_delta(row, key):
    return (sum(v[key] for v in row["resources_after"].values())
            - sum(v[key] for v in row["resources_before"].values()))


def write_report(output, outcome):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    issues = []
    environment = read_json(output / "environment.json", issues)
    options = environment.get("options", {})
    mode = options.get("mode")
    modes = MODES if mode == "compare" else (mode,) if mode in MODES else ()
    expected = options.get("trials", 0)
    reports, phases = {}, {}
    for mode in modes:
        root = output / mode
        path = root / "results.json"
        data = read_json(path, issues) if path.exists() else {}
        rows = data.get("trials", [])
        try:
            if not isinstance(rows, list):
                raise ValueError("trials must be an array")
            for number, row in enumerate(rows, 1):
                if row["mode"] != mode or row["trial"] != number:
                    raise ValueError("unexpected mode or trial order")
                for key in FIELDS[2:]:
                    if not math.isfinite(row[key]) or row[key] < 0:
                        raise ValueError(f"invalid {key}")
                if (row["files"] != options["files"] + options["variants"]
                        or row["variants"] != options["variants"]):
                    raise ValueError("unexpected cache population")
                # Validate fields used below before including this mode.
                for phase in ("baseline", "ack", "cleanup"):
                    for kind in ("ping", "hit"):
                        probe_cell([row], phase, kind)
                for key in ("Pss", "cpu_ticks", "fds"):
                    resource_delta(row, key)
        except (KeyError, TypeError, ValueError) as error:
            issues.append(f"Invalid {mode}/results.json: {error}")
            rows = []
        reports[mode] = dict(trials=rows, setup_seconds=data.get("setup_seconds"))
        phases[mode] = "unavailable"
        if (root / "progress.json").exists():
            state = read_json(root / "progress.json", issues)
            phases[mode] = f"{state.get('phase', 'unknown')} (trial {state.get('trial') or '-'})"
        if (root / "failure.json").exists():
            failure = read_json(root / "failure.json", issues)
            issues.append(f"{mode}: {failure.get('error', 'experiment failed')}")

    complete = (outcome == "success" and bool(modes) and expected > 0 and not issues
                and all(len(reports[m]["trials"]) == expected for m in modes))
    rows = [row for report in reports.values() for row in report["trials"]]
    with io.StringIO(newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
        replace_text(output / "purges.csv", stream.getvalue())
    summary = ["# Cache purge experiment", "",
        f"**{'Complete' if complete else 'INCOMPLETE'}**; experiment outcome: `{outcome}`.",
        f"Verified trials: {len(rows)}/{expected * len(modes)}.", "",
        f"Revision: `{environment.get('revision', 'unavailable')}`", "",
        "Parameters: " + ", ".join(f"{k}={options.get(k, 'unavailable')}"
            for k in ("files", "variants", "workers", "levels", "trials")) + ".", "",
        "Warm after file validation, including trial 1. Times are client-observed.",
        "ACK includes TCP connection and HTTP handling; it is not server-only time.",
        "The no-op PURGE control uses the same client without cache work. It is an",
        "HTTP round-trip floor, not a non-Vary cache purge or isolated Python cost.",
        "Do not subtract the control to claim a server execution time.", "",
        "Values are medians; ACK brackets show the minimum and maximum.",
        "File disappearance and drain are observations with roughly 20 ms polling.", "",
        "| Mode | Verified | Control ms | ACK ms [min, max] | Files gone ms | Drain ms |",
        "|---|---:|---:|---:|---:|---:|"]
    for mode, report in reports.items():
        trials = report["trials"]
        controls = [median(r["client_control_ms"]) for r in trials if r.get("client_control_ms")]
        summary.append(f"| {mode} | {len(trials)}/{expected} | {milliseconds(controls)} | "
            f"{milliseconds([r['ack_ms'] for r in trials], True)} | "
            f"{milliseconds([r['files_gone_observed_ms'] for r in trials])} | "
            f"{milliseconds([r['drained_observed_ms'] for r in trials])} |")
    summary += ["", "First and repeated trials use the same warm-file policy.", "",
        "| Mode | First ACK ms | Repeated ACK median ms | Repeated trials |",
        "|---|---:|---:|---:|"]
    for mode, report in reports.items():
        trials = report["trials"]
        summary.append(f"| {mode} | {milliseconds([r['ack_ms'] for r in trials[:1]])} | "
            f"{milliseconds([r['ack_ms'] for r in trials[1:]])} | {max(0, len(trials) - 1)} |")
    summary += ["", "Closed-loop probes: count / maximum ms across verified trials.",
        "Samples can overlap phase boundaries; do not add phase counts or average quantiles.",
        "Fresh TCP probes can miss a blocked worker when other workers accept connections.",
        "These samples are not an offered-load latency distribution or per-worker coverage.", "",
        "| Mode | Phase | Ping n / max ms | HIT n / max ms |",
        "|---|---|---:|---:|"]
    for mode, report in reports.items():
        for phase in ("baseline", "ack", "cleanup"):
            summary.append(f"| {mode} | {phase} | {probe_cell(report['trials'], phase, 'ping')} | "
                           f"{probe_cell(report['trials'], phase, 'hit')} |")
    summary += ["", "Worker resource deltas include monitoring; snapshots do not measure peaks.",
        "PSS is summed across workers; shared RSS must not be summed as private memory.", "",
        "| Mode | Median CPU ms | Median PSS delta KiB | Maximum FD delta |",
        "|---|---:|---:|---:|"]
    hz = environment.get("cpu_tick_hz")
    for mode, report in reports.items():
        trials = report["trials"]
        cpu = [resource_delta(r, "cpu_ticks") * 1000 / hz for r in trials] if hz else []
        pss = [resource_delta(r, "Pss") for r in trials]
        fds = [resource_delta(r, "fds") for r in trials]
        summary.append(f"| {mode} | {milliseconds(cpu)} | {milliseconds(pss)} | "
                       f"{max(fds) if fds else 'n/a'} |")
    summary += ["", "Setup, validation and refill time are outside PURGE timing.",
        "Cache-tree removal and process shutdown also consume the workflow budget.", "",
        "| Mode | Setup s | Validation total s | Refill total s | Last recorded phase |",
        "|---|---:|---:|---:|---|"]
    for mode, report in reports.items():
        trials = report["trials"]
        def total(key):
            return f"{sum(r[key] for r in trials):.3f}" if trials and all(key in r for r in trials) else "n/a"
        setup = report["setup_seconds"]
        setup_cell = f"{setup:.3f}" if setup is not None else "n/a"
        summary.append(f"| {mode} | {setup_cell} | {total('validation_seconds')} | "
                       f"{total('refill_seconds')} | {phases[mode]} |")
    if issues:
        summary += ["", "Report issues:", ""] + [f"- {issue}" for issue in issues]
    summary += ["", "Only trials saved after refill verification are included. Partial results",
        "do not establish that the requested experiment or process shutdown completed.",
        "Missing measurements are n/a, not zero. Inspect raw artifacts for distributions,",
        "configuration, client/clock metadata, ABI sizes and stack usage.",
        "No performance pass/fail threshold is applied.", ""]
    result = "\n".join(summary)
    replace_text(output / "summary.md", result)
    return result, complete


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--outcome", choices=("success", "failure", "cancelled", "skipped", "unknown"),
                        default="unknown")
    args = parser.parse_args()
    result, complete = write_report(args.output, args.outcome)
    print(result, end="")
    # A failed experiment remains failed in Actions. Reject a nominal success
    # whose reports are incomplete, without losing the partial summary.
    if args.outcome == "success" and not complete:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
