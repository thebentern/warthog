/*
 * Host stand-in for ESP-IDF's esp_log.h, so umac_mesh.c compiles off-target.
 * Logging is discarded; set SIMNODE_ESP_LOG to route it to stderr instead.
 */
#pragma once
#include <stdio.h>

#ifdef SIMNODE_ESP_LOG
#define SIMNODE_LOG_(lvl, tag, ...) \
    do { fprintf(stderr, "[%s %s] ", lvl, tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define SIMNODE_LOG_(lvl, tag, ...) do { (void)sizeof(tag); } while (0)
#endif

#define ESP_LOGE(tag, ...) SIMNODE_LOG_("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) SIMNODE_LOG_("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) SIMNODE_LOG_("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) SIMNODE_LOG_("D", tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) SIMNODE_LOG_("V", tag, __VA_ARGS__)
