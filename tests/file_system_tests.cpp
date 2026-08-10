#include "../cpplib/file_system.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

int main() {
    // Round-trip: write a file, read it back, byte-identical.
    const unsigned char payload_data[] = {
        'p', 'o', 'l', 'y', 'p', 'h', 'o', 'r', 'm',
        0x00, 0x01, 0xff,
        'b', 'i', 'n', 'a', 'r', 'y'
    };
    const uint32_t payload_size = sizeof(payload_data);
    const char *path = "fs_test_roundtrip.bin";

    file_system::write_file(path, (void *)payload_data, payload_size);
    File f = file_system::read_file(path);
    assert(f.data != nullptr);
    assert(f.size == payload_size);
    assert(memcmp(f.data, payload_data, payload_size) == 0);
    file_system::release_file(f);
    remove(path);

    // Missing file: data == nullptr, size == 0 (matches Win32 version's
    // failure contract: main.cpp checks file.data before use).
    File missing = file_system::read_file("no_such_file_xyz.bin");
    assert(missing.data == nullptr);
    assert(missing.size == 0);

    printf("file_system_tests: all passed\n");
    return 0;
}
