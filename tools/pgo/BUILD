load("@rules_python//python:defs.bzl", "py_binary", "py_library", "py_test")
load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

licenses(["notice"])  # Apache 2

# PGO training kit, see README.md. The scripts also run directly from a source checkout.

py_library(
    name = "gen_config_lib",
    srcs = ["gen_config.py"],
    imports = ["."],
)

py_library(
    name = "xds_churn_lib",
    srcs = ["xds_churn.py"],
    imports = ["."],
)

py_binary(
    name = "gen_config",
    srcs = ["gen_config.py"],
)

py_binary(
    name = "xds_churn",
    srcs = ["xds_churn.py"],
)

# bazel run //tools/pgo:train -- --envoy /path/to/envoy-instrumented --nighthawk /path/to/nighthawk_client
sh_binary(
    name = "train",
    srcs = ["train.sh"],
    data = [
        "gen_config.py",
        "xds_churn.py",
    ],
)

# bazel run //tools/pgo:merge -- /path/to/profiles /path/to/envoy.profdata
sh_binary(
    name = "merge",
    srcs = ["merge.sh"],
)

py_test(
    name = "gen_config_test",
    srcs = ["gen_config_test.py"],
    deps = [
        ":gen_config_lib",
        ":xds_churn_lib",
    ],
)
