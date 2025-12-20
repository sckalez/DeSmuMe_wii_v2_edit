#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize logger (safe to call multiple times)
bool SDLogger_Init(void);

// Append a formatted line to sd:/desmume_log.txt
bool SDLogger_Log(const char* fmt, ...);

// Append a formatted line to a custom filename under sd: (e.g., "renderer_log.txt")
bool SDLogger_LogFile(const char* filename, const char* fmt, ...);

// Flush (no-op; present for API completeness)
void SDLogger_Flush(void);

#ifdef __cplusplus
}
#endif
