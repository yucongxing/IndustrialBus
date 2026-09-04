# CMake install and package consumption

IndustrialBus installs a relocatable CMake Config Package for independent C++17
applications such as LVS. Tests and simulated applications are not part of the
SDK installation.

## Build and install

Configure against the locally installed Protobuf, libzmq, and cppzmq packages,
then choose a writable installation prefix:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/dependency/prefixes"
cmake --build build -j
cmake --install build --prefix "$HOME/deps/industrial-bus"
```

The installation contains:

```text
<prefix>/
├── bin/commhub
├── include/industrial_bus/...
├── include/*.pb.h
├── lib/libindustrial_bus_*.a
├── lib/cmake/IndustrialBus/
│   ├── IndustrialBusConfig.cmake
│   ├── IndustrialBusConfigVersion.cmake
│   ├── IndustrialBusTargets.cmake
│   ├── FindZeroMQ.cmake
│   └── Findcppzmq.cmake
└── share/industrial-bus/proto/*.proto
```

The generated package version accepts compatible IndustrialBus `0.1.x`
releases. It does not add global compiler flags to the consumer; exported
targets require C++17 through their usage requirements.

## Consume from LVS

A separate repository needs only the installed prefix and dependency prefixes:

```cmake
cmake_minimum_required(VERSION 3.20)
project(LVS LANGUAGES CXX)

find_package(IndustrialBus 0.1 CONFIG REQUIRED
    COMPONENTS Client Protocol Core CommHub)

add_executable(lvs src/main.cpp src/industrial_bus_adapter.cpp)
target_compile_features(lvs PRIVATE cxx_std_17)
target_link_libraries(lvs PRIVATE
    IndustrialBus::Client
    IndustrialBus::Protocol
    IndustrialBus::Core)

target_compile_definitions(lvs PRIVATE
    LVS_COMMHUB_EXECUTABLE="${IndustrialBus_COMMHUB_EXECUTABLE}")
```

Configure that repository with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/deps/industrial-bus;/path/to/dependency/prefixes"
```

The package exports these targets when their build options were enabled:

- `IndustrialBus::Core`
- `IndustrialBus::Protocol`
- `IndustrialBus::TransportSPI`
- `IndustrialBus::TransportLoopback`
- `IndustrialBus::TransportZeroMQ`
- `IndustrialBus::Bootstrap`
- `IndustrialBus::Client`
- `IndustrialBus::Integration`
- `IndustrialBus::CommHubCore`
- `IndustrialBus::Runtime`
- `IndustrialBus::CommHub`

`IndustrialBus_WITH_PROTOBUF` and `IndustrialBus_WITH_ZEROMQ` report which
optional features were present when the package was built. Requesting a target
that was not built as a required component makes `find_package` fail.

## CommHub executable

`IndustrialBus::CommHub` is an imported executable target and
`IndustrialBus_COMMHUB_EXECUTABLE` is its installed absolute path. An external
application should assign that path to `ClientOptions::hub_executable`, for
example through the compile definition above:

```cpp
industrial_bus::ClientOptions options;
options.service_name = "lvs";
options.hub_executable = LVS_COMMHUB_EXECUTABLE;
```

This is necessary when the application's executable is not installed beside
`commhub`; the runtime's empty-path fallback deliberately looks next to the
application executable.

## Dependency behavior

The Config Package discovers `Threads` and, when enabled, Protobuf, ZeroMQ, and
cppzmq. The installed `FindZeroMQ.cmake` and `Findcppzmq.cmake` preserve support
for local installations that do not provide upstream CMake Config Packages.
GoogleTest is not a consumer dependency.

IndustrialBus itself does not expose or require Qt. A Qt 5.13 application keeps
its Qt discovery and UI-thread adapter in its own repository.
