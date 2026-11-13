#!/usr/bin/env python3
"""Rewrites rds.json and cds.json in a watched directory to exercise xDS updates.

Each update is a full route configuration (200 filler routes in front of the real ones) and the
full cluster list (the real clusters plus 50 filler clusters, one of which changes per update).
Files are replaced atomically with a rename, which is what Envoy's watched_directory expects.

  xds_churn.py DIR 0      write the initial files once and exit
  xds_churn.py DIR HZ     keep rewriting both files HZ times per second until stopped
"""

import copy
import json
import os
import signal
import sys
import time

FILLER_ROUTES = 200
FILLER_CLUSTERS = 50
ROUTE_TYPE = "type.googleapis.com/envoy.config.route.v3.RouteConfiguration"
CLUSTER_TYPE = "type.googleapis.com/envoy.config.cluster.v3.Cluster"


def filler_cluster(k, version):
    # One filler cluster per update points at a different (unused) port, so CDS sees a change.
    port = 21000 + version % 1000 if k == version % FILLER_CLUSTERS else 20000 + k
    return {
        "@type": CLUSTER_TYPE,
        "name": "filler_%d" % k,
        "connect_timeout": "1s",
        "type": "STATIC",
        "load_assignment": {
            "cluster_name": "filler_%d" % k,
            "endpoints": [{
                "lb_endpoints": [{
                    "endpoint": {
                        "address": {
                            "socket_address": {
                                "address": "127.0.0.1",
                                "port_value": port
                            }
                        }
                    }
                }]
            }],
        },
    }


def write_atomic(directory, name, obj):
    tmp = os.path.join(directory, "." + name + ".tmp")
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.rename(tmp, os.path.join(directory, name))


def write(directory, base, version):
    route_config = copy.deepcopy(base["route_config"])
    route_config["@type"] = ROUTE_TYPE
    vhost = route_config["virtual_hosts"][0]
    vhost["routes"] = [{
        "match": {
            "prefix": "/filler/%d" % k
        },
        "direct_response": {
            "status": 200,
            "body": {
                "inline_string": "v%d" % version
            }
        },
    } for k in range(FILLER_ROUTES)] + vhost["routes"]

    clusters = []
    for cluster in base["clusters"]:
        cluster = copy.deepcopy(cluster)
        cluster["@type"] = CLUSTER_TYPE
        clusters.append(cluster)
    clusters += [filler_cluster(k, version) for k in range(FILLER_CLUSTERS)]

    write_atomic(directory, "rds.json", {"version_info": str(version), "resources": [route_config]})
    write_atomic(directory, "cds.json", {"version_info": str(version), "resources": clusters})


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    directory, hz = sys.argv[1], float(sys.argv[2])
    with open(os.path.join(directory, "base.json")) as f:
        base = json.load(f)
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(0))
    write(directory, base, 0)
    version = 1
    while hz > 0:
        time.sleep(1 / hz)
        write(directory, base, version)
        version += 1


if __name__ == "__main__":
    main()
