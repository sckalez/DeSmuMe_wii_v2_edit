#include "sd_logger.h"
#include <fat.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h> // for fsync
#include <sys/types.h>

static const char* DEFAULT_LOG = "desmume_log.txt";

bool SDLogger_Init(void)
{
    fatInitDefault();
    return true;
}

static int sd_write_file(const char* filename, const char* line)
{
    if (!filename || !line) return 0;
    char path[256];
    snprintf(path, sizeof(path), "sd:/%s", filename);

    FILE* f = fopen(path, "a");
    if (!f) return 0;

    size_t len = strlen(line);
    size_t wrote = fwrite(line, 1, len, f);
    fwrite("\n", 1, 1, f);
    fflush(f);

    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd); // force write-through to emulated device
    }

    fclose(f);
    return (int)(wrote == len);
}

bool SDLogger_LogFile(const char* filename, const char* fmt, ...)
{
    SDLogger_Init();

    char body[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char ts[64] = {0};
    time_t t = time(NULL);
    struct tm tm;
#if defined(_MSC_VER)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s %s", ts, body);

    return sd_write_file(filename ? filename : DEFAULT_LOG, buf) != 0;
}

bool SDLogger_Log(const char* fmt, ...)
{
    SDLogger_Init();

    char body[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char ts[64] = {0};
    time_t t = time(NULL);
    struct tm tm;
#if defined(_MSC_VER)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s %s", ts, body);

    return sd_write_file(DEFAULT_LOG, buf) != 0;
}

void SDLogger_Flush(void)
{
    // No-op: we fsync per-write in sd_write_file.
}
