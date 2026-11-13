"""Checks the configurations written by gen_config.py and xds_churn.py.

No Envoy binary is needed: the test parses the generated JSON and checks the listeners, routes,
clusters and admin settings that train.sh relies on.
"""

import json
import os
import sys
import tempfile
import unittest

from tools.pgo import gen_config
from tools.pgo import xds_churn

HTTP_PORT = 19080
H3_PORT = 19443
ORIGIN_PORT = 19081


def load(*parts):
    with open(os.path.join(*parts)) as f:
        return json.load(f)


def hcm(listener):
    filters = listener["filter_chains"][0]["filters"]
    return filters[0]["typed_config"]


def routes_by_prefix(route_config):
    return {r["match"]["prefix"]: r for r in route_config["virtual_hosts"][0]["routes"]}


def cluster_port(cluster):
    endpoint = cluster["load_assignment"]["endpoints"][0]["lb_endpoints"][0]["endpoint"]
    return endpoint["address"]["socket_address"]["port_value"]


class GenConfigTest(unittest.TestCase):

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = self._tmp.name
        self.tls_dir = os.path.join(self.out, "tls")
        argv = sys.argv
        sys.argv = [
            "gen_config.py", "--out", self.out, "--tls-dir", self.tls_dir, "--http-port",
            str(HTTP_PORT), "--h3-port",
            str(H3_PORT), "--origin-port",
            str(ORIGIN_PORT)
        ]
        try:
            gen_config.main()
        finally:
            sys.argv = argv

    def tearDown(self):
        self._tmp.cleanup()

    def assert_admin(self, config):
        # The admin interface must use a kernel-picked loopback port and no deprecated fields.
        self.assertEqual(
            config["admin"], {"address": {
                "socket_address": {
                    "address": "127.0.0.1",
                    "port_value": 0
                }
            }})

    def test_origin(self):
        config = load(self.out, "origin.json")
        self.assert_admin(config)
        listener = config["static_resources"]["listeners"][0]
        self.assertEqual(listener["address"]["socket_address"]["port_value"], ORIGIN_PORT)
        route_config = hcm(listener)["route_config"]
        self.assertGreaterEqual(route_config["max_direct_response_body_size_bytes"], 65536)
        routes = routes_by_prefix(route_config)
        for prefix, size in (("/s", 4096), ("/m", 16384), ("/l", 65536)):
            body = routes[prefix]["direct_response"]["body"]["inline_string"]
            self.assertEqual(len(body), size)

    def check_proxy_routes(self, route_config):
        self.assertEqual(len(route_config["virtual_hosts"][0]["response_headers_to_add"]), 8)
        routes = routes_by_prefix(route_config)
        self.assertEqual(sorted(routes), ["/fh", "/l", "/m", "/s"])
        for prefix in ("/s", "/m", "/l"):
            self.assertEqual(routes[prefix]["route"]["cluster"], "origin")
        fh = routes["/fh"]
        self.assertEqual(fh["direct_response"]["status"], 200)
        self.assertEqual(len(fh["response_headers_to_add"]), 52)
        # Request headers come from the client; a direct response route would ignore them.
        self.assertNotIn("request_headers_to_add", fh)

    def test_proxy(self):
        config = load(self.out, "proxy.json")
        self.assert_admin(config)
        listener = config["static_resources"]["listeners"][0]
        self.assertEqual(listener["address"]["socket_address"]["port_value"], HTTP_PORT)
        self.assertEqual(hcm(listener)["codec_type"], "AUTO")
        self.check_proxy_routes(hcm(listener)["route_config"])
        cluster = config["static_resources"]["clusters"][0]
        self.assertEqual(cluster["name"], "origin")
        self.assertEqual(cluster_port(cluster), ORIGIN_PORT)

    def test_proxy_h3(self):
        config = load(self.out, "proxy-h3.json")
        self.assert_admin(config)
        listener = config["static_resources"]["listeners"][0]
        address = listener["address"]["socket_address"]
        self.assertEqual((address["port_value"], address["protocol"]), (H3_PORT, "UDP"))
        self.assertEqual(hcm(listener)["codec_type"], "HTTP3")
        self.check_proxy_routes(hcm(listener)["route_config"])
        tls = listener["filter_chains"][0]["transport_socket"]["typed_config"]
        certificate = tls["downstream_tls_context"]["common_tls_context"]["tls_certificates"][0]
        self.assertEqual(certificate["certificate_chain"]["filename"],
                         os.path.join(self.tls_dir, "server.crt"))
        self.assertEqual(certificate["private_key"]["filename"],
                         os.path.join(self.tls_dir, "server.key"))

    def test_nighthawk_h3_transport_socket(self):
        with open(os.path.join(self.out, "nighthawk-h3-transport-socket.json")) as f:
            text = f.read()
        # nighthawk_client takes the transport socket as one command line argument.
        self.assertNotIn("\n", text)
        socket = json.loads(text)
        self.assertEqual(socket["name"], "envoy.transport_sockets.quic")
        tls = socket["typed_config"]["upstream_tls_context"]
        self.assertEqual(tls["sni"], "localhost")
        self.assertEqual(tls["common_tls_context"]["validation_context"]["trusted_ca"]["filename"],
                         os.path.join(self.tls_dir, "server.crt"))

    def test_proxy_xds(self):
        config = load(self.out, "proxy-xds.json")
        self.assert_admin(config)
        xds_dir = os.path.join(self.out, "xds")
        listener = config["static_resources"]["listeners"][0]
        self.assertEqual(listener["address"]["socket_address"]["port_value"], HTTP_PORT)
        rds = hcm(listener)["rds"]
        self.assertEqual(rds["route_config_name"], "local")
        source = rds["config_source"]["path_config_source"]
        self.assertEqual(source["path"], os.path.join(xds_dir, "rds.json"))
        self.assertEqual(source["watched_directory"]["path"], xds_dir)
        cds = config["dynamic_resources"]["cds_config"]["path_config_source"]
        self.assertEqual(cds["path"], os.path.join(xds_dir, "cds.json"))

    def test_xds_churn(self):
        xds_dir = os.path.join(self.out, "xds")
        base = load(xds_dir, "base.json")
        for version in (0, 1):
            xds_churn.write(xds_dir, base, version)
            rds = load(xds_dir, "rds.json")
            cds = load(xds_dir, "cds.json")
            self.assertEqual(rds["version_info"], str(version))
            route_config = rds["resources"][0]
            self.assertEqual(route_config["name"], "local")
            self.assertEqual(len(route_config["virtual_hosts"][0]["routes"]),
                             xds_churn.FILLER_ROUTES + 4)
            names = [c["name"] for c in cds["resources"]]
            self.assertIn("origin", names)
            self.assertEqual(len(names), xds_churn.FILLER_CLUSTERS + 1)
        self.assertFalse([n for n in os.listdir(xds_dir) if n.endswith(".tmp")])


if __name__ == "__main__":
    unittest.main()
