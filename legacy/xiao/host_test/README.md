# Host-side tests

Pure C++17 unit tests for the firmware's `domain/` layer, runnable on a
developer laptop without any ESP32 hardware. Tests the same source files
that ship in firmware.

## Build + run

```sh
cd firmware
cmake -S host_test -B host_test/build
cmake --build host_test/build -j
ctest --test-dir host_test/build --output-on-failure
```

First configure clones Catch2 v3 via FetchContent (~10 MB); subsequent
builds reuse the cached clone.

## What's tested here

Anything in `firmware/components/domain/` — by construction, those files
have no ESP-IDF dependency, so they build with the host toolchain
unmodified.

## What's NOT tested here

- HAL (depends on ESP-IDF drivers)
- Transport (libpeer, opus, TFLM — all ESP32 hot paths)
- App (FreeRTOS tasks)

Those layers get integration-tested on real hardware via the serial-log
inspection workflow in the project README.

## Adding a new test

1. Land the new domain source as `components/domain/src/<name>.cpp`.
2. Add `components/domain/test/test_<name>.cpp` with Catch2 `TEST_CASE`s.
3. Append to `host_test/CMakeLists.txt`:

   ```cmake
   add_domain_test(test_<name>
       ${DOMAIN_DIR}/src/<name>.cpp
       ${DOMAIN_DIR}/test/test_<name>.cpp)
   ```
