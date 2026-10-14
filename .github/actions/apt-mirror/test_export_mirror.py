from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from export_mirror import action_outputs, select_mirror


class MirrorExportTests(unittest.TestCase):
    def select(self, files):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for name, contents in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            return select_mirror(root)

    def test_hosted_runner_mirror_list(self):
        for name, source in (
            ("sources.list.d/ubuntu.sources", "URIs: mirror+file:/etc/apt/apt-mirrors.txt"),
            ("sources.list", "deb mirror+file:/etc/apt/apt-mirrors.txt noble main"),
        ):
            with self.subTest(source=name):
                mirror = self.select({name: source, "apt-mirrors.txt":
                    "# selected by fast-apt-mirror\nhttp://mirrors.cmich.edu/ubuntu/\tpriority:1\n"
                    "http://archive.ubuntu.com/ubuntu/\n"})
                self.assertEqual(mirror, "http://mirrors.cmich.edu/ubuntu/")
                self.assertIn("buildkit_secret=apt_mirror=" + mirror, action_outputs(mirror))

    def test_direct_deb822_and_legacy_sources(self):
        for name, source in (
            ("sources.list.d/ubuntu.sources", "  URIs: https://archive.ubuntu.com/ubuntu"),
            ("sources.list", "deb [arch=amd64 signed-by=/key] https://archive.ubuntu.com/ubuntu noble main"),
        ):
            with self.subTest(source=name):
                self.assertEqual(self.select({name: source}), "https://archive.ubuntu.com/ubuntu/")

    def test_missing_or_rejected_mirror_omits_secret(self):
        for files in (
            {},
            {"sources.list": "deb mirror+file:/etc/apt/missing noble main"},
            {"sources.list": "deb http://azure.archive.ubuntu.com/ubuntu/ noble main"},
            {"sources.list.d/ubuntu.sources": "URIs: http://security.ubuntu.com/ubuntu/"},
            {"sources.list.d/ubuntu.sources": "URIs: https://user:password@mirror.test/ubuntu/"},
            {"sources.list.d/ubuntu.sources": "URIs: https://mirror.test/ubuntu/?unsafe=value"},
        ):
            with self.subTest(files=files):
                self.assertEqual(action_outputs(self.select(files)), "mirror=\nbuildkit_secret=\n")

    def test_unreferenced_list_is_ignored(self):
        self.assertEqual(self.select({"apt-mirrors.txt": "http://stale.test/ubuntu/"}), "")


if __name__ == "__main__":
    unittest.main()
