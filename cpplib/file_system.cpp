#include "file_system.h"

#include <cstdio>
#include <cstdlib>

File file_system::read_file(const char *path)
{
    File file = {};

    FILE *fp = fopen(path, "rb");
    if (!fp) return file;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return file;
    }

    fseek(fp, 0, SEEK_SET);

    // calloc: original used HEAP_ZERO_MEMORY; shader loaders rely on the
    // buffer being usable as a NUL-terminated string when they pass
    // (char*)data with a separate length, so keep one spare zero byte.
    void *data = calloc(1, (size_t)size + 1);
    if (!data) {
        fclose(fp);
        return file;
    }

    size_t bytes_read = fread(data, 1, (size_t)size, fp);
    fclose(fp);

    if (bytes_read != (size_t)size) {
        free(data);
        return file;
    }

    file.data = data;
    file.size = (uint32_t)size;
    return file;
}

void file_system::release_file(File file)
{
    if (file.data) free(file.data);
}

uint32_t file_system::write_file(const char *path, void *data, uint32_t size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;

    size_t bytes_written = fwrite(data, 1, (size_t)size, fp);
    fclose(fp);

    return (uint32_t)bytes_written;
}
