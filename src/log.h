#pragma once

#include <string>

void log_configure(const std::wstring& file_path, bool enabled, bool verbose);
void log_shutdown();
void log_write(bool verbose_only, const char* fmt, ...);

#define LOG_INFO(...) log_write(false, __VA_ARGS__)
#define LOG_DEBUG(...) log_write(true, __VA_ARGS__)
