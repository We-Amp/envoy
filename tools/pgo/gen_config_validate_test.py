"""Runs `envoy --mode validate` on the configurations written by gen_config.py and xds_churn.py.

gen_config_test checks the shape of the generated JSON; this test checks that Envoy accepts it, so
a renamed or removed API field fails here. The routes and clusters that xds_churn.py writes for the
xds-s leg are validated in a static bootstrap of their own.

proxy-h3.json is checked with `--mode init_only` on kernel-picked ports instead: `--mode validate`
crashes on a QUIC listener (in ActiveQuicListenerFactory::initializeWorkerRouting).
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.pgo import gen_config
from tools.pgo import xds_churn

VALIDATE_CONFIGS = ("origin.json", "proxy.json", "proxy-xds.json")


def without_type(resource):
    resource = dict(resource)
    resource.pop("@type", None)
    return resource


class GenConfigValidateTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.envoy = os.path.abspath(os.environ["ENVOY_BINARY"])
        cls._tmp = tempfile.TemporaryDirectory()
        cls.out = cls._tmp.name
        tls_dir = os.path.join(cls.out, "tls")
        os.makedirs(tls_dir)
        shutil.copy(os.environ["TLS_CERT"], os.path.join(tls_dir, "server.crt"))
        shutil.copy(os.environ["TLS_KEY"], os.path.join(tls_dir, "server.key"))
        argv = sys.argv
        sys.argv = [
            "gen_config.py", "--out", cls.out, "--tls-dir", tls_dir, "--http-port", "0",
            "--h3-port", "0", "--origin-port", "19081"
        ]
        try:
            gen_config.main()
        finally:
            sys.argv = argv
        # proxy-xds.json needs the watched files to exist, as train.sh writes them before the leg.
        xds_dir = os.path.join(cls.out, "xds")
        with open(os.path.join(xds_dir, "base.json")) as f:
            cls.base = json.load(f)
        xds_churn.write(xds_dir, cls.base, 0)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_envoy(self, mode, path):
        result = subprocess.run([
            self.envoy, "--mode", mode, "-c", path, "-l", "warn", "--disable-hot-restart",
            "--concurrency", "1"
        ],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                universal_newlines=True,
                                timeout=120)
        self.assertEqual(result.returncode, 0, "%s:\n%s" % (path, result.stdout))

    def test_validate_configs(self):
        for name in VALIDATE_CONFIGS:
            with self.subTest(config=name):
                self.run_envoy("validate", os.path.join(self.out, name))

    def test_init_h3_config(self):
        self.run_envoy("init_only", os.path.join(self.out, "proxy-h3.json"))

    def test_xds_churn_resources(self):
        xds_dir = os.path.join(self.out, "xds")
        xds_churn.write(xds_dir, self.base, 1)
        with open(os.path.join(xds_dir, "rds.json")) as f:
            route_config = without_type(json.load(f)["resources"][0])
        with open(os.path.join(xds_dir, "cds.json")) as f:
            clusters = [without_type(c) for c in json.load(f)["resources"]]
        with open(os.path.join(self.out, "proxy.json")) as f:
            bootstrap = json.load(f)
        listener = bootstrap["static_resources"]["listeners"][0]
        listener["filter_chains"][0]["filters"][0]["typed_config"]["route_config"] = route_config
        bootstrap["static_resources"]["clusters"] = clusters
        path = os.path.join(self.out, "xds-static.json")
        with open(path, "w") as f:
            json.dump(bootstrap, f)
        self.run_envoy("validate", path)


if __name__ == "__main__":
    unittest.main()
