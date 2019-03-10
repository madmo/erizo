#ifndef LOG_H
#define LOG_H

void log_trace(const char* fmt, ...);
void log_info(const char* fmt, ...);
void log_error_and_abort(const char* fmt, ...);

#endif