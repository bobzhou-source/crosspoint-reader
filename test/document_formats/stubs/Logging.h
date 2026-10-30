#pragma once
#include <cstdio>
#define LOG_ERR(tag, format, ...) std::fprintf(stderr, "[%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
