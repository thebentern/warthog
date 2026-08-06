/*
 * Copyright 2026 Warthog contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux kernel API → ESP-IDF / FreeRTOS shim.
 *
 * Vendored Linux source from `morse_driver` uses kernel-isms throughout:
 *   kmalloc/kfree, spinlock_t, mutex, timer_list, sk_buff, le16_to_cpu,
 *   pr_info / printk, etc.
 *
 * This header maps those to the ESP-IDF equivalents so vendored .c files
 * can compile largely unmodified. Where 1:1 mapping isn't possible (e.g.
 * RCU, rtnl_lock) we provide a noop or assert.
 *
 * Style: mirror the kernel header names so vendored code is unchanged.
 *
 * Status: scaffold. Most macros TODO. Fill in as vendored .c files are
 * added and reveal what they reference.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

/* -------------------------------------------------------------------------
 * Type sizes — Linux uses these as proper types, kernel headers define them.
 * ESP-IDF prefers <stdint.h>. */
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;

/* GFP_* are heap allocator hint flags; on embedded we ignore them. */
#define GFP_KERNEL   0
#define GFP_ATOMIC   0
#define GFP_DMA      0

static inline void *kmalloc(size_t sz, int flags)
{
    (void)flags;
    return malloc(sz);
}
static inline void *kzalloc(size_t sz, int flags)
{
    (void)flags;
    return calloc(1, sz);
}
static inline void kfree(const void *p)
{
    free((void *)p);
}
static inline void *kmemdup(const void *src, size_t len, int flags)
{
    (void)flags;
    void *dst = malloc(len);
    if (dst) memcpy(dst, src, len);
    return dst;
}

/* TODO: krealloc, kstrdup, kstrdup_const, kasprintf, vmalloc, vfree */

/* -------------------------------------------------------------------------
 * Endianness — ESP32 is little-endian, like the chip. */
static inline u16 le16_to_cpu(u16 v) { return v; }
static inline u16 cpu_to_le16(u16 v) { return v; }
static inline u32 le32_to_cpu(u32 v) { return v; }
static inline u32 cpu_to_le32(u32 v) { return v; }
static inline u64 le64_to_cpu(u64 v) { return v; }
static inline u64 cpu_to_le64(u64 v) { return v; }
/* BE swaps from ESP-IDF */
#include "esp_byteorder.h"

/* -------------------------------------------------------------------------
 * Bit manipulation */
#ifndef BIT
#define BIT(n)        ((u32)1 << (n))
#define BIT_ULL(n)    ((u64)1 << (n))
#endif
#define GENMASK(h, l) (((~(u32)0) - ((u32)1 << (l)) + 1) & (~(u32)0 >> (31 - (h))))

/* -------------------------------------------------------------------------
 * Mutex / spinlock — FreeRTOS mutex is sufficient at task layer.
 * morse_driver uses spinlock_t with _bh variants (disable softirq); on
 * embedded we only run in task context so a mutex suffices. */
typedef SemaphoreHandle_t spinlock_t;
typedef SemaphoreHandle_t mutex_t;
#define spin_lock_init(p)    do { *(p) = xSemaphoreCreateMutex(); } while (0)
#define spin_lock_bh(p)      xSemaphoreTake(*(p), portMAX_DELAY)
#define spin_unlock_bh(p)    xSemaphoreGive(*(p))
#define spin_lock(p)         xSemaphoreTake(*(p), portMAX_DELAY)
#define spin_unlock(p)       xSemaphoreGive(*(p))
#define mutex_init(p)        do { *(p) = xSemaphoreCreateMutex(); } while (0)
#define mutex_lock(p)        xSemaphoreTake(*(p), portMAX_DELAY)
#define mutex_unlock(p)      xSemaphoreGive(*(p))

/* -------------------------------------------------------------------------
 * Timers — use FreeRTOS timers. */
struct timer_list {
    TimerHandle_t handle;
    void (*function)(struct timer_list *);
};
#define timer_setup(t, fn, flags) do { \
    (t)->function = (fn); \
    (t)->handle = xTimerCreate("kt", pdMS_TO_TICKS(1), pdFALSE, (t), \
        (TimerCallbackFunction_t)(fn)); \
} while (0)
#define mod_timer(t, expires_jiffies) \
    do { xTimerChangePeriod((t)->handle, (expires_jiffies), 0); \
         xTimerStart((t)->handle, 0); } while (0)
#define del_timer_sync(t)   xTimerDelete((t)->handle, portMAX_DELAY)
#define jiffies              ((u32)xTaskGetTickCount())
#define msecs_to_jiffies(ms) pdMS_TO_TICKS(ms)

/* -------------------------------------------------------------------------
 * Logging — route through ESP_LOG. The vendored code uses MORSE_INFO,
 * MORSE_WARN, MORSE_ERR macros and pr_info/pr_err — redefine these in
 * compatibility headers. */
#define HALOW_MESH_TAG "halow_mesh"
#define pr_info(fmt, ...)  ESP_LOGI(HALOW_MESH_TAG, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  ESP_LOGW(HALOW_MESH_TAG, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)   ESP_LOGE(HALOW_MESH_TAG, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) ESP_LOGD(HALOW_MESH_TAG, fmt, ##__VA_ARGS__)
#define printk(level, fmt, ...) ESP_LOGI(HALOW_MESH_TAG, fmt, ##__VA_ARGS__)

/* -------------------------------------------------------------------------
 * Annotation-only macros — drop them */
#define __packed                __attribute__((packed))
#define __aligned(a)            __attribute__((aligned(a)))
#define __must_check
#define __always_inline         inline __attribute__((always_inline))
#define unlikely(x)             (x)
#define likely(x)               (x)
#define BUILD_BUG_ON(cond)      _Static_assert(!(cond), "BUILD_BUG_ON")
#define WARN_ON(cond)           ((cond) ? (ESP_LOGW(HALOW_MESH_TAG, "WARN_ON(%s) at %s:%d", #cond, __FILE__, __LINE__), 1) : 0)
#define WARN_ON_ONCE(cond)      WARN_ON(cond)
#define BUG_ON(cond)            do { if (cond) abort(); } while (0)

/* -------------------------------------------------------------------------
 * RCU — no concurrent-RCU on single-core FreeRTOS, mutex suffices.
 * The morse_driver only uses RCU for sta_info list iteration; we can
 * substitute with the mutex-protected list. */
#define rcu_read_lock()         do {} while (0)
#define rcu_read_unlock()       do {} while (0)
#define rcu_dereference(p)      (p)
#define rcu_assign_pointer(p, v) ((p) = (v))
#define synchronize_rcu()       do {} while (0)
#define list_for_each_entry_rcu list_for_each_entry  /* TODO: define list_for_each_entry */

/* -------------------------------------------------------------------------
 * Workqueue / delayed work — replace with FreeRTOS tasks. */
struct work_struct {
    void (*func)(struct work_struct *);
};
struct delayed_work {
    struct work_struct work;
    /* TODO: timer handle */
};
#define INIT_WORK(w, fn)         ((w)->func = (fn))
#define INIT_DELAYED_WORK(w, fn) ((w)->work.func = (fn))
/* schedule_work and friends — TODO: implement via a worker task or
 * direct timer. */

/* -------------------------------------------------------------------------
 * netdev / sk_buff — major. The morselib already uses `struct mmpkt`
 * which is its sk_buff equivalent. The shim aliases sk_buff to mmpkt
 * for vendored .c file compatibility. */
typedef struct mmpkt sk_buff_t;
#define sk_buff sk_buff_t
/* TODO: skb_put, skb_pull, skb_push, skb_data, skb_len, dev_kfree_skb_any */

/* -------------------------------------------------------------------------
 * ETH_ALEN and MAC helpers */
#define ETH_ALEN 6
static inline int is_zero_ether_addr(const u8 *a) {
    return (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]) == 0;
}
static inline int is_broadcast_ether_addr(const u8 *a) {
    return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff;
}
static inline int is_multicast_ether_addr(const u8 *a) {
    return a[0] & 0x01;
}
static inline void eth_broadcast_addr(u8 *a) {
    memset(a, 0xff, ETH_ALEN);
}
static inline int ether_addr_equal(const u8 *a, const u8 *b) {
    return memcmp(a, b, ETH_ALEN) == 0;
}

/* -------------------------------------------------------------------------
 * cfg80211 / mac80211 stubs — major. Vendored code references types
 * like struct ieee80211_vif, ieee80211_hw, ieee80211_sta, ieee80211_ops.
 * We define minimal opaque versions and our umac_mesh wires the
 * relevant fields at boundaries.
 *
 * TODO: define struct ieee80211_vif { void *drv_priv; ... }
 *       define struct ieee80211_sta { u8 addr[ETH_ALEN]; ... }
 *       define struct ieee80211_hw { void *priv; ... }
 *       Many mac80211 inline helpers can be copied verbatim from
 *       /usr/include/linux/ieee80211.h since they're pure math.
 */
