#ifndef CPBM_LOG_H
#define CPBM_LOG_H

/* Appends to cpbm-identity.log next to the DLL. Truncated once per launch, so
 * the file always describes the session you are actually looking at. */
void cpbm_log_open(void);
void cpbm_log(const char *fmt, ...);
void cpbm_log_close(void);

#endif
