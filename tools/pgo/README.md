# PGO training for Envoy

Scripts to build Envoy with clang profile-guided optimization (PGO) and ThinLTO. The full
recipe, its caveats and measured results are in
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
$ bazel build --config=pgo --fdo_optimize=/tmp/envoy.profdata //source/exe:envoy-static
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

| Leg     | Protocol | Path  | Response                                                |
|---------|----------|-------|---------------------------------------------------------|
| `h1-s`  | HTTP/1.1 | `/s`  | 4 KiB body, proxied                                     |
| `h1-fh` | HTTP/1.1 | `/fh` | direct response, 57 request and 52 response headers added |
| `h1-m`  | HTTP/1.1 | `/m`  | 16 KiB body, proxied                                    |
| `h1-l`  | HTTP/1.1 | `/l`  | 64 KiB body, proxied                                    |
| `h2-s`  | HTTP/2   | `/s`  | as `h1-s`                                               |
| `h2-fh` | HTTP/2   | `/fh` | as `h1-fh`                                              |
| `h3-s`  | HTTP/3   | `/s`  | as `h1-s`                                               |
| `h3-fh` | HTTP/3   | `/fh` | as `h1-fh`                                              |
| `xds-s` | HTTP/1.1 | `/s`  | as `h1-s`, while routes and clusters change (see below) |

The proxied paths go to an origin, which is a second Envoy process serving direct responses.
Use a normal, non-instrumented Envoy for it with `--origin-envoy`; by default the instrumented
binary is reused, and its profile goes to `origin-profile/`, which is never merged.

The HTTP/3 legs use a throwaway self-signed ECDSA certificate that `train.sh` generates in
`OUT/tls/` with `openssl`.

In the `xds-s` leg, Envoy reads its routes (RDS) and clusters (CDS) from watched files.
`xds_churn.py` rewrites both files `CHURN_HZ` times per second (default 2): a full route
configuration with 200 filler routes, and the cluster list with 50 filler clusters, one of
which changes each time.

Choose legs with `--legs h1-s,h2-s,...`. Train on every protocol and feature you serve: code
that never runs during training is optimized as cold code.

## Options

Run `tools/pgo/train.sh --help` for the full list. Environment variables:

* `CONCURRENCY`, `RPS`, `CONNECTIONS`: load shape (defaults 4, 500, 4).
* `CHURN_HZ`: xDS update rate in the `xds-s` leg (default 2).
* `ENVOY_CPUS`, `ORIGIN_CPUS`, `CLIENT_CPUS`: CPU lists for `taskset`. Unset means no pinning.
  On a machine with several NUMA nodes or many cores, keeping the three apart gives a cleaner
  profile and more stable training.
* `ADMIN_PORT`, `HTTP_PORT`, `H3_PORT`, `ORIGIN_PORT`, `ORIGIN_ADMIN_PORT`: loopback ports
  (defaults 19901, 19080, 19443, 19081, 19902).

All processes bind to `127.0.0.1`. The script stops only the processes it started.

## Output

* `profiles/*.profraw`: the training profiles, one per leg.
* `legs.txt`: successful and completed requests per leg, plus the xDS update counts.
* `logs/`: Envoy and Nighthawk output for each leg.
* `*.json`, `xds/`: the generated Envoy configurations (from `gen_config.py`).

Each `.profraw` file is about 35 MB. Use a fresh `--out` directory for each training run.
