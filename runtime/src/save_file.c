#include "tarzan_recomp.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int tz_atomic_write(const char* path, const void* data, size_t n) {
    if (!path || !path[0] || !data) return -1;
    char tmp[540];
    int nw = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (nw <= 0 || nw >= (int)sizeof(tmp)) return -1;
    FILE* fp = fopen(tmp, "wb");
    if (!fp) return -1;
    size_t wrote = fwrite(data, 1, n, fp);
    if (fflush(fp) != 0) {
        fclose(fp);
        remove(tmp);
        return -1;
    }
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    if (fclose(fp) != 0 || wrote != n) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}
