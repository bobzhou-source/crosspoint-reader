#pragma once

template <typename... Args>
inline void fontCacheManagerVectorTestLog(const Args&...) {}

#define LOG_ERR(...) fontCacheManagerVectorTestLog(__VA_ARGS__)
#define LOG_INF(...) fontCacheManagerVectorTestLog(__VA_ARGS__)
#define LOG_DBG(...) fontCacheManagerVectorTestLog(__VA_ARGS__)
