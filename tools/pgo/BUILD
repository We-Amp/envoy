load("@rules_python//python:defs.bzl", "py_library", "py_test")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

licenses(["notice"])  # Apache 2

# PGO training kit, see README.md. The scripts also run directly from a source checkout.
# train.sh runs gen_config.py and xds_churn.py with the host python3 by design: they only use
# the standard library.

py_library(
    name = "gen_config_lib",
    srcs = ["gen_config.py"],
)

py_library(
    name = "xds_churn_lib",
    srcs = ["xds_churn.py"],
)

# bazel run //tools/pgo:train -- --envoy /path/to/envoy-instrumented \
#     --nighthawk /path/to/nighthawk_client --out /tmp/pgo-train
sh_binary(
    name = "train",
    srcs = ["train.sh"],
    data = [
        ":gen_config_lib",
        ":xds_churn_lib",
    ],
)

# bazel run //tools/pgo:merge -- /path/to/profiles /path/to/envoy.profdata
sh_binary(
    name = "merge",
    srcs = ["merge.sh"],
    data = ["@llvm_toolchain_llvm//:profdata"],
    env = {"LLVM_PROFDATA_RUNFILE": "$(rootpath @llvm_toolchain_llvm//:profdata)"},
)

py_test(
    name = "gen_config_test",
    srcs = ["gen_config_test.py"],
    deps = [
        ":gen_config_lib",
        ":xds_churn_lib",
    ],
)

# Runs `envoy --mode validate` on every generated configuration, so a renamed or removed API
# field fails here. Manual, because it builds //source/exe:envoy-static:
# bazel test //tools/pgo:gen_config_validate_test
py_test(
    name = "gen_config_validate_test",
    srcs = ["gen_config_validate_test.py"],
    data = [
        "//source/exe:envoy-static",
        "//test/config/integration/certs:upstreamlocalhostcert.pem",
        "//test/config/integration/certs:upstreamlocalhostkey.pem",
    ],
    env = {
        "ENVOY_BINARY": "$(rootpath //source/exe:envoy-static)",
        "TLS_CERT": "$(rootpath //test/config/integration/certs:upstreamlocalhostcert.pem)",
        "TLS_KEY": "$(rootpath //test/config/integration/certs:upstreamlocalhostkey.pem)",
    },
    tags = ["manual"],
    deps = [
        ":gen_config_lib",
        ":xds_churn_lib",
    ],
)
