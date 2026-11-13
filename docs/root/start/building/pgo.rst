.. _start_building_pgo:

Building with profile-guided optimization
=========================================

Envoy can be built with clang profile-guided optimization (PGO) plus ThinLTO. No source code
changes are needed. The compiler uses a profile recorded from a real run to lay out hot code
together, move cold paths out of the way and guide inlining across translation units.

In our measurements, this reduced CPU time per request by roughly a quarter to a third on
HTTP/1 and HTTP/2 proxy workloads, compared with a regular ``-c opt`` build of the same commit.
The setup was a dual-socket x86-64 server at a fixed clock, with Envoy on one socket at 4 to 16
workers and an open-loop Nighthawk client and the origin on the other. How much you gain depends
on your traffic and on how well the training traffic matches it, so measure with your own
configuration before relying on a number.

The profile is passed with Bazel's native ``--fdo_optimize`` flag, so it is a tracked build
input: a changed profile rebuilds everything that depends on it.

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
fixed mix of nine one-minute legs at 4 workers, each sending a browser-like set of about 20
request headers:

* HTTP/1 requests proxied to an origin that returns 4 KiB, 16 KiB and 64 KiB bodies.
* HTTP/1 requests answered by a direct response that adds 60 response headers.
* HTTP/2 and HTTP/3 versions of the 4 KiB proxied request and of the direct response.
* HTTP/1 4 KiB proxied requests while routes and clusters are replaced through file-based xDS.

It takes about ten minutes.

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

  $ bazel build --config=thinlto --fdo_optimize=/tmp/envoy.profdata //source/exe:envoy-static

``--fdo_optimize`` applies the profile and needs an absolute path. ``--config=thinlto`` turns
on Bazel's ``thin_lto`` feature. Without ``--fdo_optimize`` you get a ThinLTO build only, with
no profile applied. In the same measurements, ``--fdo_optimize`` without ThinLTO gave about two
thirds of the gain. Such a build does not get the profile mismatch flags of ``--config=thinlto``
(see *Profile staleness* below); pass them as ``--copt`` options to use a profile from another
commit.

Caveats
-------

* **ThinLTO backend actions.** With the ``thin_lto`` feature, Bazel compiles each object to
  bitcode, runs a short indexing link, and then optimizes each object again in its own backend
  action before the final link. There are as many backend actions as objects, several thousand
  for ``envoy-static``, but they are cached like compiles, and the final link needs little
  memory. Split debug info (``--fission``) works as in a regular build.
* **Binary size.** Build ``//source/exe:envoy-static.stripped`` to ship a binary without debug
  information.
* **Profile staleness.** A profile describes the code it was recorded from, but it does not
  have to come from the same commit: a profile recorded on another commit builds and still
  helps. Functions that changed since the profile was recorded fall back to normal
  optimization; they are not miscompiled, but they lose the benefit. ``--config=thinlto``
  turns off the profile mismatch warnings (``-Wno-profile-instr-unprofiled``,
  ``-Wno-profile-instr-out-of-date`` and ``-Wno-backend-plugin``), which would otherwise fail
  the ``-Werror`` build. In the same measurements, a profile recorded 11 days or four weeks away
  from the built commit kept roughly four fifths or more of the same-commit gain.

  Retrain periodically, for example once per release, and when your traffic changes.
* **Coverage of the training mix.** Code that never ran during training is optimized as cold
  code. Train on every protocol and feature you serve. In the same measurements HTTP/3 improved
  much less when it was left out of the training run than when it was included.
* **Reproducibility.** The optimized binary depends on the profile as well as the source. Keep
  the profile with the build if you need to rebuild the same binary later.
