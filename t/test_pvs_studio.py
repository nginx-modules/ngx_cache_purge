"""Check the analysis pipeline's failure handling without a PVS license."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

TOOL = r'''#!/usr/bin/env python3
import json, os, pathlib, re, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
stage = name
if name == 'pvs-studio-analyzer':
    stage = args[0]
elif name == 'plog-converter':
    stage = args[args.index('-t') + 1]
with open(os.environ['PVS_TEST_CALLS'], 'a') as log:
    log.write(json.dumps([stage, args]) + '\n')
if os.environ.get('PVS_TEST_FAIL') == stage:
    sys.exit(23)
if stage != 'credentials' and any(os.environ.get(key) for key in
        ('LICENSE_NAME', 'LICENSE_KEY')):
    sys.exit(24)
if stage == 'trace':
    sys.exit(subprocess.call(args[args.index('--') + 1:]))
if stage == 'configure':
    module = next(a.split('=', 1)[1] for a in args if a.startswith('--add-module='))
    config = (pathlib.Path(module) / 'config').read_text()
    sources = re.search(r'CACHE_PURGE_SRCS="([^"]+)"', config, re.S).group(1)
    units = re.findall(r'\$ngx_addon_dir/(ngx_cache_purge_\w+)\.c', sources)
    rules = ['objs/addon/fixture/' + unit + '.o: ' + module + '/' + unit + '.c'
             for unit in units]
    if os.environ.get('PVS_TEST_MISSING_TARGET'):
        rules.pop()
    pathlib.Path('objs').mkdir(exist_ok=True)
    pathlib.Path('objs/Makefile').write_text('\n'.join(rules) + '\nobjs/core.o: core.c\n')
if stage in ('analyze', 'tasklist', 'json', 'sarif'):
    path = pathlib.Path(args[args.index('-o') + 1]) if '-o' in args else \
        pathlib.Path(args[args.index('-n') + 1] + '.' + stage)
    if os.environ.get('PVS_TEST_EMPTY_SARIF') and stage == 'sarif':
        path.touch()
    else:
        path.write_text('fixture report\n')
'''


class AnalysisPipeline(unittest.TestCase):
    def run_pipeline(self, fail='', stale=False, jobs='2', empty_sarif=False,
                     git_source=False, license_name='fixture-name',
                     license_key='fixture-key', missing_target=False):
        with tempfile.TemporaryDirectory(prefix='pvs test ') as directory:
            base = Path(directory)
            nginx, reports, tools = (base / name for name in ('nginx', 'reports', 'bin'))
            for path in (nginx, reports, tools):
                path.mkdir()
            configure = nginx / ('auto/configure' if git_source else 'configure')
            configure.parent.mkdir(exist_ok=True)
            configure.write_text(TOOL)
            configure.chmod(0o755)
            for name in ('make', 'pvs-studio-analyzer', 'plog-converter'):
                path = tools / name
                path.write_text(TOOL)
                path.chmod(0o755)
            calls = base / 'calls'
            if stale:
                (reports / 'pvs-studio.sarif').write_text('old result')
            env = dict(os.environ, NGINX_SOURCE=str(nginx), PVS_REPORT_DIR=str(reports),
                       PVS_JOBS=jobs, PVS_TEST_CALLS=str(calls), PVS_TEST_FAIL=fail,
                       LICENSE_NAME=license_name,
                       LICENSE_KEY=license_key,
                       PVS_TEST_MISSING_TARGET='1' if missing_target else '',
                       PVS_TEST_EMPTY_SARIF='1' if empty_sarif else '',
                       PATH=str(tools) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(['bash', str(ROOT / 't/pvs_studio.sh')],
                                    cwd=base, env=env, capture_output=True, text=True)
            invocations = [json.loads(line) for line in calls.read_text().splitlines()] \
                if calls.exists() else []
            return result, invocations

    def test_traces_forced_rebuild_and_keeps_module_outside_exclusion(self):
        for git_source in (False, True):
            with self.subTest(git_source=git_source):
                result, calls = self.run_pipeline(git_source=git_source)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([stage for stage, _ in calls],
                                 ['credentials', 'configure', 'trace', 'make',
                                  'analyze', 'tasklist', 'json', 'sarif'])
                args = dict(calls)
                self.assertEqual(args['credentials'],
                                 ['credentials', 'fixture-name', 'fixture-key'])
                self.assertIn('--add-module=' + str(ROOT), args['configure'])
                self.assertIn('--with-threads', args['configure'])
                self.assertEqual(args['make'][:4], ['-f', 'objs/Makefile', '-B', '-j2'])
                config = (ROOT / 'config').read_text()
                sources = re.search(r'CACHE_PURGE_SRCS="([^"]+)"', config, re.S).group(1)
                units = re.findall(r'\$ngx_addon_dir/(ngx_cache_purge_\w+)\.c', sources)
                self.assertEqual(args['make'][4:],
                                 ['objs/addon/fixture/' + unit + '.o' for unit in units])
                excluded = args['analyze'][args['analyze'].index('-e') + 1]
                self.assertFalse(str(ROOT).startswith(excluded + '/'))
                self.assertIn('GA:1,2', args['sarif'])
                self.assertEqual(args['json'][args['json'].index('-R') + 1], 'toRelative')
                self.assertEqual(args['json'][args['json'].index('-r') + 1], str(ROOT))
                self.assertEqual(args['sarif'][args['sarif'].index('-r') + 1], 'file://')
                self.assertEqual(args['sarif'][-1], 'relative.json')

    def test_each_failed_stage_stops_the_pipeline(self):
        stages = ['credentials', 'configure', 'trace', 'make', 'analyze', 'tasklist', 'json', 'sarif']
        for stage in stages:
            with self.subTest(stage=stage):
                result, calls = self.run_pipeline(fail=stage)
                self.assertEqual(result.returncode, 23, result.stderr)
                self.assertEqual([name for name, _ in calls], stages[:stages.index(stage) + 1])

    def test_missing_module_object_stops_before_tracing(self):
        result, calls = self.run_pipeline(missing_target=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expected one object target', result.stderr)
        self.assertEqual([name for name, _ in calls], ['credentials', 'configure'])

    def test_stale_reports_are_not_reused(self):
        result, calls = self.run_pipeline(stale=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must be empty', result.stderr)
        self.assertEqual(calls, [])

    def test_missing_license_stops_before_any_tool_runs(self):
        for values in ({'license_name': ''}, {'license_key': ''}):
            with self.subTest(values=values):
                result, calls = self.run_pipeline(**values)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])

    def test_empty_sarif_fails(self):
        result, _ = self.run_pipeline(empty_sarif=True)
        self.assertNotEqual(result.returncode, 0)

    def test_invalid_parallelism_fails_before_build(self):
        for jobs in ('0', '-1', 'two'):
            with self.subTest(jobs=jobs):
                result, calls = self.run_pipeline(jobs=jobs)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])


class ImageRepository(unittest.TestCase):
    @unittest.skipUnless(shutil.which('apt-cache'), 'APT parser is unavailable')
    def test_image_repository_is_accepted_by_apt(self):
        dockerfile = (ROOT / 't/Dockerfile').read_text()
        stage = dockerfile.split(' AS pvs-studio\n', 1)[1].split('\nFROM ', 1)[0]
        setup = 'curl --fail' + stage.split('    curl --fail', 1)[1]
        setup = setup.split('    apt-get $ca_option update', 1)[0]
        setup = setup.replace('\\\n', '').strip().removesuffix('&&')

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            tools, parts, lists = (base / name for name in ('bin', 'parts', 'lists'))
            for path in (tools, parts, lists):
                path.mkdir()
            source = base / 'pvs-studio.list'
            setup = setup.replace('/etc/apt/sources.list.d/pvs-studio.list',
                                  '"$PVS_TEST_APT_SOURCE"')
            # Reproduce the vendor's existing options without network access.
            # Public-key download/import are outside this syntax regression.
            curl = tools / 'curl'
            curl.write_text('#!/usr/bin/env python3\n'
                            'import pathlib, sys\n'
                            'if any(a.endswith("/viva64.list") for a in sys.argv):\n'
                            '    pathlib.Path(sys.argv[sys.argv.index("-o") + 1]).write_text(\n'
                            '        "deb [arch=amd64] https://files.pvs-studio.com/deb "\n'
                            '        "viva64-release pvs-studio\\n")\n')
            curl.chmod(0o755)
            gpg = tools / 'gpg'
            gpg.write_text('#!/bin/sh\nexit 0\n')
            gpg.chmod(0o755)
            env = dict(os.environ, PVS_TEST_APT_SOURCE=str(source),
                       PATH=str(tools) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(['sh', '-eu', '-c', setup], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('signed-by=/usr/share/keyrings/pvs-studio.gpg',
                          source.read_text())
            self.assertEqual(source.read_text().split()[-3:],
                             ['https://files.pvs-studio.com/deb',
                              'viva64-release', 'pvs-studio'])
            result = subprocess.run([
                'apt-cache', '-o', 'Dir::Etc::sourcelist=' + str(source),
                '-o', 'Dir::Etc::sourceparts=' + str(parts),
                '-o', 'Dir::State::lists=' + str(lists),
                '-o', 'Dir::State::status=/dev/null',
                '-o', 'Dir::Cache::pkgcache=', '-o', 'Dir::Cache::srcpkgcache=',
                'policy'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


class ModuleTargets(unittest.TestCase):
    def test_rejects_empty_or_ambiguous_module_targets(self):
        manifest = 'CACHE_PURGE_SRCS="$ngx_addon_dir/ngx_cache_purge_module.c"'
        rule = 'objs/addon/src/ngx_cache_purge_module.o: module.c\n'
        cases = [('', rule), ('CACHE_PURGE_SRCS=""', rule),
                 ('CACHE_PURGE_SRCS="$ngx_addon_dir/unsupported.cc"', rule),
                 (manifest, ''), (manifest, rule + rule)]
        for config, makefile in cases:
            with self.subTest(config=config, makefile=makefile), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'config').write_text(config)
                (root / 'Makefile').write_text(makefile)
                result = subprocess.run(['perl', str(ROOT / 't/pvs_targets.pl'),
                                         str(root / 'config'), str(root / 'Makefile')],
                                        capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
