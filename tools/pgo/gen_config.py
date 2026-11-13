#!/usr/bin/env python3
"""Writes the Envoy configurations used by train.sh.

All files are JSON, so only the Python standard library is needed:

  origin.json    a plain Envoy acting as the upstream; direct responses for /s, /m and /l.
  proxy.json     the instrumented Envoy: HTTP/1 and HTTP/2 (AUTO codec) on one TCP listener.
  proxy-h3.json  the instrumented Envoy: HTTP/3 on a UDP listener.
  proxy-xds.json the instrumented Envoy: HTTP/1 with routes (RDS) and clusters (CDS) read from
                 watched files, rewritten by xds_churn.py.
  xds/base.json  the route configuration and clusters that xds_churn.py starts from.
"""

import argparse
import json
import os

# Body sizes served by the origin for the proxied paths.
BODY_SIZES = {"/s": 4096, "/m": 16384, "/l": 65536}

# The many-headers path (/fh) is a direct response on the proxy itself, with many response
# headers added. Request headers come from the client (see REQUEST_HEADERS in train.sh).
FH_RESPONSE_HEADERS = 52
FH_BODY_SIZE = 128
VHOST_RESPONSE_HEADERS = 8

HCM_TYPE = ("type.googleapis.com/"
            "envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager")
ROUTER = {
    "name": "envoy.filters.http.router",
    "typed_config": {
        "@type": "type.googleapis.com/envoy.extensions.filters.http.router.v3.Router"
    },
}


def headers(prefix, count, value):
    return [{
        "header": {
            "key": "%s-%02d" % (prefix, i),
            "value": value(i)
        }
    } for i in range(count)]


def admin():
    # Port 0: the kernel picks a free port, which Envoy writes to --admin-address-path.
    return {"address": {"socket_address": {"address": "127.0.0.1", "port_value": 0}}}


def tcp_address(port):
    return {"socket_address": {"address": "127.0.0.1", "port_value": port}}


def origin_cluster(port):
    return {
        "name": "origin",
        "connect_timeout": "1s",
        "type": "STATIC",
        "load_assignment": {
            "cluster_name": "origin",
            "endpoints": [{
                "lb_endpoints": [{
                    "endpoint": {
                        "address": tcp_address(port)
                    }
                }]
            }],
        },
    }


def proxy_route_config():
    routes = [{"match": {"prefix": p}, "route": {"cluster": "origin"}} for p in BODY_SIZES]
    routes.append({
        "match": {
            "prefix": "/fh"
        },
        "response_headers_to_add": headers("x-origin", FH_RESPONSE_HEADERS, lambda i: "v" * 62),
        "direct_response": {
            "status": 200,
            "body": {
                "inline_string": "a" * FH_BODY_SIZE
            }
        },
    })
    return {
        "name":
            "local",
        "virtual_hosts": [{
            "name":
                "service",
            "domains": ["*"],
            "response_headers_to_add":
                headers("x-fixed", VHOST_RESPONSE_HEADERS, lambda i: "fixed-value-%02d" % i),
            "routes":
                routes,
        }],
    }


def hcm(codec, route_specifier, extra=None):
    config = {"@type": HCM_TYPE, "stat_prefix": "ingress_http", "codec_type": codec}
    config.update(route_specifier)
    config["http_filters"] = [ROUTER]
    if extra:
        config.update(extra)
    return {"name": "envoy.filters.network.http_connection_manager", "typed_config": config}


def origin(args):
    routes = [{
        "match": {
            "prefix": p
        },
        "direct_response": {
            "status": 200,
            "body": {
                "inline_string": "a" * size
            }
        },
    } for p, size in BODY_SIZES.items()]
    route_config = {
        "name": "origin",
        "max_direct_response_body_size_bytes": max(BODY_SIZES.values()),
        "virtual_hosts": [{
            "name": "origin",
            "domains": ["*"],
            "routes": routes
        }],
    }
    return {
        "admin": admin(),
        "static_resources": {
            "listeners": [{
                "name": "origin",
                "address": tcp_address(args.origin_port),
                "filter_chains": [{
                    "filters": [hcm("AUTO", {"route_config": route_config})]
                }],
            }]
        },
    }


def proxy(args):
    return {
        "admin": admin(),
        "static_resources": {
            "listeners": [{
                "name": "proxy",
                "address": tcp_address(args.http_port),
                "filter_chains": [{
                    "filters": [hcm("AUTO", {"route_config": proxy_route_config()})]
                }],
            }],
            "clusters": [origin_cluster(args.origin_port)],
        },
    }


def proxy_h3(args):
    tls = {
        "common_tls_context": {
            "tls_certificates": [{
                "certificate_chain": {
                    "filename": os.path.join(args.tls_dir, "server.crt")
                },
                "private_key": {
                    "filename": os.path.join(args.tls_dir, "server.key")
                },
            }]
        }
    }
    return {
        "admin": admin(),
        "static_resources": {
            "listeners": [{
                "name": "proxy_h3",
                "address": {
                    "socket_address": {
                        "address": "127.0.0.1",
                        "port_value": args.h3_port,
                        "protocol": "UDP"
                    }
                },
                "udp_listener_config": {
                    "quic_options": {},
                    "downstream_socket_config": {
                        "prefer_gro": True
                    }
                },
                "filter_chains": [{
                    "transport_socket": {
                        "name": "envoy.transport_sockets.quic",
                        "typed_config": {
                            "@type": ("type.googleapis.com/envoy.extensions.transport_sockets."
                                      "quic.v3.QuicDownstreamTransport"),
                            "downstream_tls_context": tls,
                        },
                    },
                    "filters": [
                        hcm("HTTP3", {"route_config": proxy_route_config()},
                            {"http3_protocol_options": {}})
                    ],
                }],
            }],
            "clusters": [origin_cluster(args.origin_port)],
        },
    }


def proxy_xds(args):
    xds_dir = os.path.join(args.out, "xds")

    def source(name):
        return {
            "path_config_source": {
                "path": os.path.join(xds_dir, name),
                "watched_directory": {
                    "path": xds_dir
                },
            },
            "resource_api_version": "V3",
        }

    return {
        "admin": admin(),
        "node": {
            "id": "pgo-train",
            "cluster": "pgo-train"
        },
        "static_resources": {
            "listeners": [{
                "name": "proxy",
                "address": tcp_address(args.http_port),
                "filter_chains": [{
                    "filters": [
                        hcm("HTTP1",
                            {"rds": {
                                "route_config_name": "local",
                                "config_source": source("rds.json")
                            }})
                    ]
                }],
            }]
        },
        "dynamic_resources": {
            "cds_config": source("cds.json")
        },
    }


def write(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="output directory (absolute)")
    parser.add_argument("--tls-dir", required=True, help="directory with server.crt/server.key")
    parser.add_argument("--http-port", type=int, required=True)
    parser.add_argument("--h3-port", type=int, required=True)
    parser.add_argument("--origin-port", type=int, required=True)
    args = parser.parse_args()

    os.makedirs(os.path.join(args.out, "xds"), exist_ok=True)
    write(os.path.join(args.out, "origin.json"), origin(args))
    write(os.path.join(args.out, "proxy.json"), proxy(args))
    write(os.path.join(args.out, "proxy-h3.json"), proxy_h3(args))
    write(os.path.join(args.out, "proxy-xds.json"), proxy_xds(args))
    write(os.path.join(args.out, "xds", "base.json"), {
        "route_config": proxy_route_config(),
        "clusters": [origin_cluster(args.origin_port)],
    })


if __name__ == "__main__":
    main()
