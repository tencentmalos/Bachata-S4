// Standalone: c++ -std=c++20 tests/host_runtime/pkg_block_io_tests.cpp -o /tmp/pkg-block-io-tests
#include "../../android/shadps4-app/core/runtime/src/main/cpp/pkg/pkg_block_io.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace bachata_pkg;
    assert(PfsBlockCount(0) == 0);
    assert(PfsWriteBytes(0, 0) == 0);
    for (uint64_t boundary : {uint64_t{1} << 31, uint64_t{1} << 32, uint64_t{9} << 30}) {
        for (uint64_t tail : {uint64_t{1}, uint64_t{59402}, PfsBlockBytes - 1}) {
            const auto size = boundary + tail;
            assert(PfsBlockCount(size) == boundary / PfsBlockBytes + 1);
            assert(PfsWriteBytes(size, boundary) == tail);
            assert(PfsWriteBytes(size, boundary - PfsBlockBytes) == PfsBlockBytes);
            assert(PfsWriteBytes(size, size) == 0);
            assert(PfsWriteBytes(size, size + 1) == 0);
        }
        assert(PfsBlockCount(boundary) == boundary / PfsBlockBytes);
        assert(PfsWriteBytes(boundary, boundary - PfsBlockBytes) == PfsBlockBytes);
    }
    std::puts("PKG block I/O: 2/4/9 GiB boundaries and partial tails passed");
}
