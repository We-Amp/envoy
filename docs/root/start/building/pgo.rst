.. _start_building_pgo:

Building with profile-guided optimization
=========================================

Envoy can be built with clang profile-guided optimization (PGO) plus ThinLTO. No source code
changes are needed. The compiler uses a profile recorded from a real run to lay out hot code
together, move cold paths out of the way and guide inlining across translation units.

On the HTTP/1 and HTTP/2 proxy workloads we measured, this reduced CPU time per request by
roughly a quarter to a third compared with a regular ``-c opt`` build, and improved p99 latency
slightly. How much you gain depends on your traffic and on how well the training traffic
matches it, so measure with your own configuration before relying on a number.

The build uses Bazel's native ``--fdo_optimize`` flag, so the profile is a tracked build input
and remote and disk caches stay correct when the profile changes.

Requirements
------------

* The default clang toolchain. These steps do not apply to ``--config=gcc`` builds.
* `Nighthawk <https://github.com/envoyproxy/nighthawk>`_ (``nighthawk_client``), ``curl``,
  ``openssl`` and ``python3`` for the training script.
* A Linux host for training. The training script only uses the loopback interface.

**Step 1: Build an instrumented binary**

.. code-block:: console

  $ bazel build --config=pgo-instrument //source/exe:envoy-static
  $ cp bazel-bin/source/exe/envoy-static /tmp/envoy-instrumented

Copy the binary out of ``bazel-bin``, because the optimized build in step 4 replaces it.

**Step 2: Train**

Run the instrumented binary under representative traffic. :repo:`tools/pgo/train.sh` runs a
fixed mix of nine one-minute legs at 4 workers: HTTP/1 small, medium and large proxied bodies,
a many-headers direct response, HTTP/2 and HTTP/3 small and many-headers, and HTTP/1 while
routes and clusters are updated through file-based xDS. It takes about ten minutes.

.. code-block:: console

  $ tools/pgo/train.sh --envoy /tmp/envoy-instrumented \
      --nighthawk /path/to/nighthawk_client --out /tmp/pgo-train

To train on your own traffic instead, start the instrumented binary with
``LLVM_PROFILE_FILE=/some/dir/envoy-%p-%m.profraw`` in its environment, send it traffic, and
stop it with ``POST /quitquitquit`` on the admin interface. The profile is only written when
Envoy exits cleanly.

See :repo:`tools/pgo/README.md` for the options of the training script.

**Step 3: Merge the profiles**

The profile must be merged with ``llvm-profdata`` from the same LLVM version as the compiler.
:repo:`tools/pgo/merge.sh` finds the one in Envoy's hermetic toolchain:

.. code-block:: console

  $ tools/pgo/merge.sh /tmp/pgo-train/profiles /tmp/envoy.profdata

**Step 4: Build the optimized binary**

.. code-block:: console

  $ bazel build --config=pgo --fdo_optimize=/tmp/envoy.profdata //source/exe:envoy-static

``--fdo_optimize`` needs an absolute path. ``--config=pgo`` adds ThinLTO
(``-flto=thin``) and ``--fission=no``. Using ``--fdo_optimize`` without ThinLTO gives about
two thirds of the gain.

Caveats
-------

* **Memory.** ThinLTO moves code generation into the link step, so linking ``envoy-static``
  and large test binaries needs much more memory than a regular build. If links are killed
  for running out of memory, lower ``--jobs`` or limit Bazel with, for example,
  ``--local_resources=memory=HOST_RAM*.6``.
* **Binary size.** With ``--fission=no`` the debug information stays in the binary, which can
  then be several gigabytes. Strip it, or keep the debug information separately, before
  shipping it. A large unstripped binary is also slow to start from a cold page cache.
* **Profile staleness.** A profile describes the code it was recorded from. Retrain when the
  code changes substantially, or when your traffic changes. Functions that changed since the
  profile was recorded fall back to normal optimization; they are not miscompiled, but they
  lose the benefit.
* **Coverage of the training mix.** Code that never ran during training is optimized as cold
  code. Train on every protocol and feature you serve. In our measurements HTTP/3 improved
  much less when it was left out of the training run than when it was included.
* **Reproducibility.** The optimized binary depends on the profile as well as the source. Keep
  the profile with the build if you need to rebuild the same binary later.
