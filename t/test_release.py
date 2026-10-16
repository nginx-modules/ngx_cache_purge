"""Exercise release publication ordering and failure handling without GitHub writes."""
import json
import os
import textwrap
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

GH = r'''#!/usr/bin/env python3
import json, os, pathlib, shutil, sys
root = pathlib.Path(os.environ['FAKE_GH_ROOT'])
args = sys.argv[1:]
with (root / 'calls').open('a') as log:
    log.write(json.dumps(args) + '\n')
mode = os.environ.get('FAKE_GH_MODE', '')
command = args[1]
if command == 'view':
    if mode == 'new': sys.exit(1)
    print('false' if mode == 'published' else 'true')
elif command == 'upload':
    if mode == 'upload-failed': sys.exit(1)
    for name in args[3:]:
        if name != '--clobber': shutil.copy2(name, root / 'remote' / pathlib.Path(name).name)
elif command == 'download':
    dest = pathlib.Path(args[args.index('--dir') + 1])
    for source in (root / 'remote').iterdir(): shutil.copy2(source, dest / source.name)
    if mode == 'corrupt':
        next(dest.glob('*.zip')).write_bytes(b'corrupted')
    if mode == 'extra': (dest / 'unexpected.txt').write_text('stale asset')
'''


class ReleasePublication(unittest.TestCase):
    def run_release(self, mode='', missing=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'bin').mkdir()
            (root / 'remote').mkdir()
            (root / 'dist').mkdir()
            gh = root / 'bin' / 'gh'
            gh.write_text(GH)
            gh.chmod(0o755)
            assets = [f'ngx_cache_purge-v4.0.0-nginx-{v}-linux-{a}.{e}'
                      for v in ('1.30.6', '1.31.7')
                      for a in ('x86_64', 'aarch64') for e in ('tar.gz', 'zip')]
            for name in assets[1:] if missing else assets:
                (root / 'dist' / name).write_bytes(name.encode())
            env = dict(os.environ, RELEASE_TAG='v4.0.0', FAKE_GH_ROOT=tmp,
                       FAKE_GH_MODE=mode, PATH=f'{root / "bin"}:{os.environ["PATH"]}')
            result = subprocess.run(['bash', str(ROOT / 'tools/publish-release.sh')],
                                    cwd=root, env=env, capture_output=True, text=True)
            calls = (root / 'calls').read_text() if (root / 'calls').exists() else ''
            return result, calls

    def test_new_release_published_only_after_download_verification(self):
        result, calls = self.run_release('new')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"--draft"', calls)
        self.assertLess(calls.index('"download"'), calls.index('"edit"'))

    def test_existing_draft_resumes(self):
        result, calls = self.run_release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('"create"', calls)
        self.assertIn('"edit"', calls)

    def test_published_release_is_not_modified(self):
        result, calls = self.run_release('published')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('"upload"', calls)
        self.assertNotIn('"edit"', calls)

    def test_incomplete_matrix_makes_no_api_calls(self):
        result, calls = self.run_release(missing=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, '')

    def test_failure_or_unexpected_assets_prevent_publication(self):
        for mode in ('upload-failed', 'corrupt', 'extra'):
            with self.subTest(mode=mode):
                result, calls = self.run_release(mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('"edit"', calls)


class ValgrindGate(unittest.TestCase):
    def test_selected_valgrind_cannot_be_skipped(self):
        workflow = (ROOT / '.github/workflows/build-test-release.yml').read_text()
        gate = workflow.split("python3 - <<'PYTHON'\n", 1)[1].split('          PYTHON', 1)[0]
        gate = textwrap.dedent(gate)
        for event, full, run, memory, expected in (
            ('pull_request', False, True, 'success', 0),
            ('pull_request', False, True, 'skipped', 1),
            ('push', False, True, 'skipped', 0),
            ('push', True, True, 'skipped', 1),
            ('pull_request', False, False, 'skipped', 0),
            ('pull_request', False, True, 'failure', 1),
        ):
            with self.subTest(event=event, full=full, run=run, memory=memory):
                jobs = {
                    'prepare': {'result': 'success', 'outputs': {
                        'run': str(run).lower(), 'full': str(full).lower()}},
                    'memory-leak-test': {'result': memory},
                    'test-matrix': {'result': 'success' if run else 'skipped'},
                }
                with tempfile.NamedTemporaryFile() as summary:
                    env = dict(os.environ, RESULTS=json.dumps(jobs), EVENT_NAME=event,
                               GITHUB_STEP_SUMMARY=summary.name)
                    result = subprocess.run(['python3', '-c', gate], env=env,
                                            capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stderr)


class RebuildCheck(unittest.TestCase):
    def test_archive_difference_blocks_release(self):
        for content, expected in (('identical', 0), ('changed', 1)):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'tools').mkdir()
                (root / 'dist').mkdir()
                for name in ('module.tar.gz', 'module.zip'):
                    (root / 'dist' / name).write_text('identical')
                (root / 'tools/build-release-module.sh').write_text(
                    '#!/bin/bash\nset -eu\n'
                    'printf %s "$REBUILD_CONTENT" > "$RELEASE_OUTPUT_DIR/module.tar.gz"\n'
                    'printf %s "$REBUILD_CONTENT" > "$RELEASE_OUTPUT_DIR/module.zip"\n')
                summary = root / 'summary'
                env = dict(os.environ, REBUILD_CONTENT=content, NGINX_VERSION='1.31.7',
                           GITHUB_STEP_SUMMARY=str(summary))
                result = subprocess.run(['bash', str(ROOT / 'tools/verify-release-rebuild.sh')],
                                        cwd=root, env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertIn('PASS' if expected == 0 else 'FAIL', summary.read_text())


if __name__ == '__main__':
    unittest.main()
