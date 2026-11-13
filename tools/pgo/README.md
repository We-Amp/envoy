# PGO training for Envoy

Scripts to build Envoy with clang profile-guided optimization (PGO) and ThinLTO. The full
recipe and its caveats are in
[docs/root/start/building/pgo.rst](../../docs/root/start/building/pgo.rst).

## Steps

```console
# 1. Instrumented build.
$ bazel build --config=pgo-instrument //source/exe:envoy-static
$ cp bazel-bin/source/exe/envoy-static /tmp/envoy-instrumented

# 2. Train. Takes about ten minutes with the default 60 s legs.
$ tools/pgo/train.sh --envoy /tmp/envoy-instrumented \
    --nighthawk /path/to/nighthawk_client --out /tmp/pgo-train

# 3. Merge with the toolchain's own llvm-profdata.
$ tools/pgo/merge.sh /tmp/pgo-train/profiles /tmp/envoy.profdata

# 4. Optimized build.
$ bazel build --config=thinlto --fdo_optimize=/tmp/envoy.profdata //source/exe:envoy-static
```

Copy the instrumented binary out of `bazel-bin` before step 4, since the optimized build
replaces it. Pass the same Bazel startup options (for example `--output_base`) to every step;
`merge.sh` takes them in `BAZEL_STARTUP_OPTS`, or set `LLVM_PROFDATA` directly.

## What `train.sh` runs

Each leg starts a fresh instrumented Envoy, sends open-loop traffic with
[Nighthawk](https://github.com/envoyproxy/nighthawk) for `--duration` seconds (default 60), and
stops Envoy through `POST /quitquitquit` so the profile is written. One `.profraw` file is
written per leg. The default is 4 Envoy workers and 4 Nighthawk workers at 500 requests per
second each.

On every leg the client sends the same browser-like request header set (20 headers: user agent,
accept headers, a cookie, a referer, `sec-*` client hints, `x-forwarded-*` and `x-request-id`),
so request header parsing and HPACK/QPACK decoding are trained on a realistic request.

| Leg     | Protocol | Path  | What Envoy does                                              |
|---------|----------|-------|--------------------------------------------------------------|
| `h1-s`  | HTTP/1.1 | `/s`  | proxies to the origin over HTTP/1.1; 4 KiB response body     |
| `h1-fh` | HTTP/1.1 | `/fh` | direct response that adds 60 response headers; 128 B body    |
| `h1-m`  | HTTP/1.1 | `/m`  | as `h1-s` with a 16 KiB body                                 |
| `h1-l`  | HTTP/1.1 | `/l`  | as `h1-s` with a 64 KiB body                                 |
| `h2-s`  | HTTP/2   | `/s`  | as `h1-s`, downstream HTTP/2 (cleartext)                     |
| `h2-fh` | HTTP/2   | `/fh` | as `h1-fh`, downstream HTTP/2 (cleartext)                    |
| `h3-s`  | HTTP/3   | `/s`  | as `h1-s`, downstream HTTP/3                                 |
| `h3-fh` | HTTP/3   | `/fh` | as `h1-fh`, downstream HTTP/3                                |
| `xds-s` | HTTP/1.1 | `/s`  | as `h1-s`, while routes and clusters are replaced (see below) |

The 60 response headers on `/fh` are 8 added on the virtual host and 52 on the route. The
upstream connection to the origin is always HTTP/1.1.

The proxied paths go to an origin, which is a second Envoy process serving direct responses.
Use a normal, non-instrumented Envoy for it with `--origin-envoy`; by default the instrumented
binary is reused, and its profile goes to `origin-profile/`, which is never merged.

The HTTP/3 legs use a throwaway self-signed ECDSA certificate that `train.sh` generates in
`OUT/tls/` with `openssl`.

In the `xds-s` leg, Envoy reads its routes (RDS) and clusters (CDS) from watched files.
`xds_churn.py` rewrites both files `CHURN_HZ` times per second (default 2): a full route
configuration with 200 filler routes in front of the real ones, and the cluster list with 50
filler clusters, one of which changes each time. Both files live in one watched directory, so
each rewrite reloads both; `legs.txt` reports these reloads.

The mix does not cover TLS over TCP, upstream HTTP/2, gRPC, access logging or HTTP filters other
than the router.

Choose legs with `--legs h1-s,h2-s,...`. Train on every protocol and feature you serve: code
that never runs during training is optimized as cold code.

## Options

Run `tools/pgo/train.sh --help` for the full list. Environment variables:

* `CONCURRENCY`, `RPS`, `CONNECTIONS`: load shape (defaults 4, 500, 4).
* `CHURN_HZ`: xDS update rate in the `xds-s` leg (default 2).
* `ENVOY_CPUS`, `ORIGIN_CPUS`, `CLIENT_CPUS`: CPU lists for `taskset`. Unset means no pinning.
  On a machine with several NUMA nodes or many cores, keeping the three apart gives a cleaner
  profile and more stable training.
* `HTTP_PORT`, `H3_PORT`, `ORIGIN_PORT`: loopback listener ports (defaults 19080, 19443 UDP,
  19081). The script fails before starting anything if one of them is in use (the UDP check
  needs `ss`).

All processes bind to `127.0.0.1`. Each Envoy's admin interface uses a port picked by the
kernel, which Envoy writes to `logs/*.admin` (`--admin-address-path`); the script talks only to
those addresses, and stops only the processes it started.

A leg fails if any response is not a 2xx, or if fewer than 90% of the expected requests
(`RPS` x `CONCURRENCY` x `--duration`) succeeded.

## Output

* `profiles/*.profraw`: the training profiles, one per leg.
* `legs.txt`: successful, completed and expected requests per leg, plus the xDS reload counts.
* `logs/`: Envoy and Nighthawk output for each leg.
* `*.json`, `xds/`: the generated Envoy configurations (from `gen_config.py`).

Each `.profraw` file is about 35 MB. Use a fresh `--out` directory for each training run.
