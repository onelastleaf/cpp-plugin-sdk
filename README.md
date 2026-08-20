# onelastleaf C++ plugin SDK

Use this SDK to build an [onelastleaf](https://github.com/onelastleaf/onelastleaf)
plugin in C++20. You register named actions; the SDK handles the connection,
handshake, jobs, cancellation, host calls, and shutdown protocol for you.

If you want to build a plugin, start with the generated project below. If you
are changing the SDK itself, jump to [Work on the SDK](#work-on-the-sdk).

## Before you start

You will need:

- CMake 3.24 or newer
- a C++20 compiler
- pkg-config
- OpenSSL development files
- protobuf development files and `protoc`
- gRPC C++ development files and `grpc_cpp_plugin`

CMake will point out anything it cannot find. In particular, protobuf and gRPC
need to provide the `protobuf` and `grpc++` pkg-config modules.

## Create your first plugin

Let oll create the project skeleton. It includes a pinned SDK dependency, a
small `echo` action, a test, and the `oll.toml` manifest used for installation:

```sh
oll plugin new my-plugin \
  --language cpp \
  --id com.example.my-plugin \
  --name my-plugin

cmake -S my-plugin -B my-plugin/build -DCMAKE_BUILD_TYPE=Release
cmake --build my-plugin/build --parallel
ctest --test-dir my-plugin/build --output-on-failure
```

Use a new destination directory: `plugin new` will not overwrite one that is
already there. The command only writes the project files. It does not initialize
Git, install the plugin, or access the network. The first CMake configure is
when the pinned SDK is downloaded.

## Add the SDK to an existing CMake project

Already have a CMake project? Fetch a specific SDK release and link its
namespaced target:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my-plugin VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
  onelastleaf_plugin_sdk
  GIT_REPOSITORY https://github.com/onelastleaf/cpp-plugin-sdk.git
  GIT_TAG v0.1.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(onelastleaf_plugin_sdk)

add_executable(my-plugin src/main.cpp)
target_compile_features(my-plugin PRIVATE cxx_std_20)
target_link_libraries(my-plugin PRIVATE onelastleaf::plugin_sdk)
install(TARGETS my-plugin RUNTIME DESTINATION bin)
```

Keep `GIT_TAG` pinned so builds are reproducible. You can pin a commit instead;
remove `GIT_SHALLOW TRUE` if that commit is not the tip of a fetched tag.

Here is the smallest useful `src/main.cpp`: register an action, then hand control
to the SDK with `run()`.

```cpp
#include <string>
#include <utility>
#include <vector>

#include <onelastleaf/plugin_sdk.hpp>

int main() {
  onelastleaf::Plugin plugin{"com.example.my-plugin", "0.1.0"};

  plugin.action(
      "echo", "Return the supplied arguments",
      [](onelastleaf::ActionContext &,
         const std::vector<std::string> &arguments) {
        std::string output;
        for (const auto &argument : arguments) {
          if (!output.empty())
            output += ' ';
          output += argument;
        }
        return onelastleaf::ActionResult::string(std::move(output));
      });

  return plugin.run();
}
```

Keep the ID passed to `Plugin` identical to `plugin.id` in `oll.toml`. oll may
run more than one action at a time, so write handlers with concurrency in mind.
When an action needs more than its arguments, `ActionContext` gives it
cancellation and deadline information, configuration access, host calls,
structured logging, and artifact publishing. See
[`examples/conformance.cpp`](examples/conformance.cpp) for working examples.

`context.host()` returns a cheap value that is safe to copy during the action.
It is tied to that job, though: calls made after the action finishes, after the
job is cancelled, or after the plugin session closes fail with an exception.
`Plugin::run()` likewise represents one process session and may only be called
once on a `Plugin` object.

For a large artifact, do not read the whole file into a `vector` first. Give the
SDK the number of chunks and a function that reads one chunk at a time:

```cpp
context.host().store_artifact(
    context.trace(), context.job_id(), descriptor, chunk_count,
    [&input](std::uint32_t) {
      std::string chunk;
      // Read at most context.host().maximum_artifact_chunk_bytes() bytes.
      read_next_chunk(input, chunk);
      return chunk;
    });
```

The source is called once for each index from zero to `chunk_count - 1`.
Chunks must be nonempty and must match the size and SHA-256 in the descriptor.
A zero-byte artifact uses `chunk_count == 0`; in that case the source may be an
empty function.

## Package the plugin for oll

oll installs a plugin from a Git remote and looks for `oll.toml` at the repository
root. The generated project already has the right file. If you are wiring up an
existing project, this is the equivalent manifest for the example above:

```toml
format_version = 1

[plugin]
id = "com.example.my-plugin"
name = "my-plugin"

[source]
checkout = "source"
steps = [
  [
    "cmake", "-S", "{source}", "-B", "{source}/build",
    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX={install}",
  ],
  [
    "cmake", "--build", "{source}/build", "--target", "install",
    "--parallel",
  ],
]

[source.dependencies]
"cmake" = "Install CMake, a C++ compiler, protobuf, and gRPC."

[runtime]
argv = ["{install}/bin/my-plugin"]
```

Each recipe command is an argv array, not a shell command. oll fills in the path
placeholders, builds from a private source checkout, and writes the installed
result into a candidate that is renamed into an immutable generation. It only
launches the executable from the published generation.

This SDK follows the canonical protobuf wire contract. It never computes,
embeds, publishes, or compares a schema hash or fingerprint. Descriptor-wide
hashes change for compatible additions and unrelated services, so they reject
valid peers. Protocol changes instead preserve field numbers and wire types,
give additions safe absent semantics, and tolerate unknown fields. Exact SDK
pins provide reproducible builds; they are not protobuf API versioning.

There is no protocol-wide size cap for `PluginEnvelope`. The SDK configures
both gRPC directions as unlimited instead of inheriting gRPC's smaller receive
default. Artifact chunks remain bounded by the limit negotiated in
`HostHello`, so large artifacts still use the streaming artifact API.

Commit the project and push it to a Git remote that the machine running oll can
reach. For a new oll deployment, initialize it, finish the generated
configuration, and start the daemon:

```sh
oll init NODE_NAME
# Complete the generated deployment configuration, then choose one:
oll run
# or: oll start
```

If you chose foreground `oll run`, open another shell for the plugin commands:

```sh
oll plugin install https://github.com/example/my-plugin.git --source
oll plugin start my-plugin
oll plugin call my-plugin echo -- hello from cpp
```

A fresh install starts out stopped, giving you a chance to inspect it before any
code runs. `plugin start` records that the plugin should stay running.
`plugin call` prints a job ID as soon as the plugin accepts the work. Use that ID
to check the result:

```sh
oll job info JOB_ID
oll plugin log my-plugin
oll plugin info my-plugin
```

After pushing a new commit on a tracked branch, publish it with
`oll plugin update my-plugin`. An update does not interrupt the process that is
already running; use `oll plugin restart my-plugin` when you are ready to switch
to the new build.

## Work on the SDK

If you are contributing to this repository, configure, build, and run its unit
tests directly:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DONELASTLEAF_PLUGIN_SDK_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

SDK tests are opt-in. Keeping them off for normal consumers also keeps CMake's
reserved `test` build target out of their projects, so a plugin may itself be
named `test`.

The conformance plugin is not part of the default build. Build it explicitly
when you need it:

```sh
cmake --build build --target plugin-sdk-conformance --parallel
```

This target is a real plugin executable, not a test server. The shared
conformance suite runs it through a matching oll host.

### Install the CMake package locally

You can also install the SDK into a private prefix:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install"
cmake --build build --parallel
cmake --install build
```

Point a consumer at that prefix with
`-DCMAKE_PREFIX_PATH=/absolute/path/to/build/install`, then use the installed
package like this:

```cmake
find_package(onelastleaf-plugin-sdk 0.1 CONFIG REQUIRED)
target_link_libraries(my-plugin PRIVATE onelastleaf::plugin_sdk)
```

The installed package still uses the protobuf, gRPC, OpenSSL, and pkg-config
dependencies from the system.

## How the plugin process fits into oll

One detail is easy to get backwards: oll hosts the loopback gRPC server, and the
plugin connects to it as a client. oll chooses an ephemeral port, launches the
plugin, and sets `OLL_PLUGIN_ENDPOINT` for that process. You should not set the
variable yourself or launch the installed executable by hand.

stdin also belongs to the runtime. oll keeps it open as a parent-liveness pipe;
when the SDK sees EOF, it knows the parent is gone and exits. Send application
input through actions or host calls instead. Anything the plugin writes to
stdout or stderr is captured in its per-plugin log.
