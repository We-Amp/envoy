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
replaces it. Run directly, `merge.sh` looks for `llvm-profdata` in the Bazel output base: pass the
same Bazel startup options (for example `--output_base`) as for the build in
`BAZEL_STARTUP_OPTS`, or set `LLVM_PROFDATA`. Under `bazel run //tools/pgo:merge` it uses the
toolchain's `@llvm_toolchain_llvm//:profdata` instead.

The profile does not have to come from the commit you build. Functions that changed since it
was recorded fall back to normal optimization, and `--config=thinlto` silences the mismatch
warnings that would otherwise fail the `-Werror` build (an `--fdo_optimize` build without
`--config=thinlto` needs those flags on its own command line). In our measurements, a profile
recorded 11 days or four weeks away from the built commit kept roughly four fifths or more of the
same-commit gain. Retrain periodically, for example once per release.

The scripts run directly from a source checkout, or through Bazel:

```console
$ bazel run //tools/pgo:train -- --envoy /tmp/envoy-instrumented \
    --nighthawk /path/to/nighthawk_client --out /tmp/pgo-train
$ bazel run //tools/pgo:merge -- /tmp/pgo-train/profiles /tmp/envoy.profdata
$ bazel test //tools/pgo:gen_config_test
$ bazel test //tools/pgo:gen_config_validate_test
```

`gen_config_test` checks the generated configurations without running Envoy.
`gen_config_validate_test` runs `envoy --mode validate` on each of them, so it catches a renamed or
removed API field; it builds `//source/exe:envoy-static` and is therefore tagged `manual`.

## Requirements

* Linux and bash 4.4 or newer.
* `curl`, `openssl` (1.1.1 or newer) and `python3`; `ss` for the UDP port check and `taskset` for
  CPU pinning are optional.
* `nighthawk_client` from [Nighthawk](https://github.com/envoyproxy/nighthawk), built from source
  or taken from the `envoyproxy/nighthawk-dev` image. The HTTP/3 legs need a build with HTTP/3
  support.

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
configuration with 200 filler routes in front of the real ones (on purpose, so route matching
over a larger table is trained), and the cluster list with 50
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
(`RPS` x `CONCURRENCY` x `--duration`) succeeded. Each leg writes its profile to `staging/LEG/`,
and it is moved to `profiles/` only when the leg passes, so a failed run never leaves the profile
of the failed leg where `merge.sh` would pick it up. A failed run stops at that leg. To finish
it, run the remaining legs with `--legs` into a new `--out` directory and move their profiles
into the first run's `profiles/` before merging.

## Output

* `profiles/*.profraw`: the training profiles, one per leg that passed.
* `staging/`: only after a failed leg, its profile, which is not merged.
* `legs.txt`: successful, completed and expected requests per leg, plus the xDS reload counts.
* `logs/`: Envoy and Nighthawk output for each leg.
* `*.json`, `xds/`: the generated Envoy configurations (from `gen_config.py`).

Each `.profraw` file is about 35 MB. Use a fresh `--out` directory for each training run.
