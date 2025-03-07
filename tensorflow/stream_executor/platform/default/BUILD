licenses(["notice"])  # Apache 2.0

package(default_visibility = ["//tensorflow/stream_executor:__subpackages__"])

load("//tensorflow:tensorflow.bzl", "tf_copts", "if_windows")

load("//third_party/dml:build_defs.bzl", "if_dml")

cc_library(
    name = "platform",
    textual_hdrs = [
        "initialize.h",
    ],
    deps = ["//tensorflow/core:lib"],
)

cc_library(
    name = "dso_loader",
    srcs = ["dso_loader.cc"] + select({
        # include dynamic loading checker only for open source build
        "//tensorflow:oss": ["dlopen_checker.cc"],
        "//conditions:default": ["dlopen_checker_stub.cc"],
    }),
    hdrs = ["dso_loader.h"],
    copts = tf_copts(),
    deps = [
        "//tensorflow/stream_executor:platform",
        "//tensorflow/stream_executor/lib",
        "//tensorflow/stream_executor/platform",
        "@com_google_absl//absl/strings",
        "@local_config_cuda//cuda:cuda_headers",
        "@local_config_tensorrt//:tensorrt_headers",
    ] + if_dml([
        "@dml_redist//:headers",
    ]),
    linkopts = if_windows(["-DEFAULTLIB:pathcch.lib"]) 
)
