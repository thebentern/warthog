/*
 * Copyright 2021-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mmosal.h"
#include "mmhal_os.h"
#include "warthog_assert.h"
#include "warthog_shim.h"

/* --------------------------------------------------------------------------------------------- */

/** Maximum number of failure records to store (must be a power of 2). */
#define MAX_FAILURE_RECORDS 4

_Static_assert(MAX_FAILURE_RECORDS == WARTHOG_ASSERT_RECORDS_MAX, "AT+ASSERT? reads every record");

/** Fast implementation of _x % _m where _m is a power of 2. */
#define FAST_MOD(_x, _m) ((_x) & ((_m) - 1))

/** Data structure for assertion information to be preserved. */
struct mmosal_preserved_failure_info
{
    /** Magic number, to check if the info is valid. */
    uint32_t magic;

    /** Number of failures recorded. */
    uint32_t failure_count;

    /** Number of most recently displayed failure. */
    uint32_t displayed_failure_count;

    /** Preserved information from the most recent failure(s). */
    struct mmosal_failure_info info[MAX_FAILURE_RECORDS];
};

/** Magic number to put in @c mmosal_assert_info.magic to indicate that the assertion info
 *  is valid. */
#define ASSERT_INFO_MAGIC (0xabcd1234)

/* Persistent assertion info. Linker script should put this into memory that is not
 * zeroed on boot. Be careful to update linker script if renaming. */
struct mmosal_preserved_failure_info preserved_failure_info __attribute__((section(".noinit")));

void mmosal_log_failure_info(const struct mmosal_failure_info *info)
{
    uint32_t record_num;

    if (preserved_failure_info.magic != ASSERT_INFO_MAGIC)
    {
        preserved_failure_info.failure_count = 0;
        preserved_failure_info.displayed_failure_count = 0;
    }

    preserved_failure_info.magic = ASSERT_INFO_MAGIC;
    record_num = FAST_MOD(preserved_failure_info.failure_count, MAX_FAILURE_RECORDS);
    preserved_failure_info.failure_count++;
    memcpy(&preserved_failure_info.info[record_num], info, sizeof(*info));
    /* This port's mmport.h reads no PC: the return address here is the assert's call site. */
    if (info->pc == 0)
    {
        preserved_failure_info.info[record_num].pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    }
}

uint32_t warthog_assert_records(struct mmosal_failure_info *out, uint32_t max, uint32_t *kept)
{
    *kept = 0;
    if (preserved_failure_info.magic != ASSERT_INFO_MAGIC)
    {
        return 0;
    }
    const uint32_t count = preserved_failure_info.failure_count;
    uint32_t n = count < MAX_FAILURE_RECORDS ? count : MAX_FAILURE_RECORDS;
    n = n < max ? n : max;
    for (uint32_t i = 0; i < n; i++)
    {
        out[i] = preserved_failure_info.info[FAST_MOD(count - n + i, MAX_FAILURE_RECORDS)];
    }
    *kept = n;
    return count;
}

void warthog_assert_clear(void)
{
    preserved_failure_info.magic = 0;
    preserved_failure_info.failure_count = 0;
    preserved_failure_info.displayed_failure_count = 0;
}

/** The panic reason the core dump keeps (AT+COREDUMP?): the newest record's pc, fileid and line. */
static char s_assert_reason[64];

static char *assert_put_(char *p, const char *s)
{
    while (*s != '\0')
    {
        *p++ = *s++;
    }
    return p;
}

static char *assert_put_hex_(char *p, uint32_t v)
{
    for (int sh = 28; sh >= 0; sh -= 4)
    {
        *p++ = "0123456789abcdef"[(v >> sh) & 0xfu];
    }
    return p;
}

/* Formatted by hand: no libc call on a path an ISR or a critical section can take. */
static const char *assert_reason_(void)
{
    char *p = assert_put_(s_assert_reason, "MMOSAL_ASSERT");
    if (preserved_failure_info.magic == ASSERT_INFO_MAGIC && preserved_failure_info.failure_count != 0)
    {
        const struct mmosal_failure_info *r =
            &preserved_failure_info.info[FAST_MOD(preserved_failure_info.failure_count - 1u,
                                                  MAX_FAILURE_RECORDS)];
        char d[10];
        int n = 0;
        uint32_t v = r->line;
        p = assert_put_hex_(assert_put_(p, " pc=0x"), r->pc);
        p = assert_put_hex_(assert_put_(p, " fileid=0x"), r->fileid);
        p = assert_put_(p, " line=");
        do
        {
            d[n++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v != 0u && n < 10);
        while (n > 0)
        {
            *p++ = d[--n];
        }
    }
    *p = '\0';
    return s_assert_reason;
}

/* No console output (TinyUSB owns the console's USB PHY) and no scheduler call (an ISR or a critical
 * section can assert): the panic handler writes the core dump, then resets the board. */
void mmosal_impl_assert(void)
{
    esp_system_abort(assert_reason_());
}

/* --------------------------------------------------------------------------------------------- */

void *mmosal_malloc_(size_t size)
{
    return pvPortMalloc(size);
}

#ifdef MMOSAL_TRACK_ALLOCATIONS
void *mmosal_malloc_dbg(size_t size, const char *name, unsigned line_number)
{
    return pvPortMalloc_dbg(size, name, line_number);
}
#else
void *mmosal_malloc_dbg(size_t size, const char *name, unsigned line_number)
{
    (void)name;
    (void)line_number;
    return pvPortMalloc(size);
}
#endif

void mmosal_free(void *p)
{
    vPortFree(p);
}

void *mmosal_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

void *mmosal_calloc(size_t nitems, size_t size)
{
    void *ptr = pvPortMalloc(nitems * size);
    if (ptr == NULL)
    {
        return NULL;
    }

    memset(ptr, 0, nitems * size);
    return ptr;
}

/* --------------------------------------------------------------------------------------------- */

/* warthog: AT+STACKS? for the tasks started here; a chip restart ends and restarts drv, spi_irq, health. */
#define WARTHOG_TASK_SLOTS 8

static struct
{
    char name[16];
    TaskHandle_t live;
    uint32_t exit_min; /* least free stack (bytes) an instance had as it exited; UINT32_MAX none */
} s_task_slots[WARTHOG_TASK_SLOTS];
static portMUX_TYPE s_task_slots_lock = portMUX_INITIALIZER_UNLOCKED;

/* The slot named @p name, taken if new; -1 when full. Lock held. */
static int task_slot_(const char *name, bool take)
{
    int free_slot = -1;
    for (int i = 0; i < WARTHOG_TASK_SLOTS; i++)
    {
        if (s_task_slots[i].name[0] == '\0')
        {
            free_slot = free_slot < 0 ? i : free_slot;
        }
        else if (strncmp(s_task_slots[i].name, name, sizeof(s_task_slots[i].name) - 1) == 0)
        {
            return i;
        }
    }
    if (take && free_slot >= 0)
    {
        strncpy(s_task_slots[free_slot].name, name, sizeof(s_task_slots[free_slot].name) - 1);
        s_task_slots[free_slot].exit_min = UINT32_MAX;
        return free_slot;
    }
    return -1;
}

static void task_stack_enter_(void)
{
    const char *name = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_task_slots_lock);
    const int i = task_slot_(name, true);
    if (i >= 0)
    {
        s_task_slots[i].live = xTaskGetCurrentTaskHandle();
    }
    portEXIT_CRITICAL(&s_task_slots_lock);
}

/* The task's last act before it deletes itself, so a reader holding the lock never sees a freed TCB. */
static void task_stack_exit_(void)
{
    const uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_task_slots_lock);
    for (int i = 0; i < WARTHOG_TASK_SLOTS; i++)
    {
        if (s_task_slots[i].live == me)
        {
            s_task_slots[i].live = NULL;
            s_task_slots[i].exit_min = hwm < s_task_slots[i].exit_min ? hwm : s_task_slots[i].exit_min;
        }
    }
    portEXIT_CRITICAL(&s_task_slots_lock);
}

bool warthog_task_stack(const char *name, uint32_t *live, uint32_t *exit_min)
{
    *live = UINT32_MAX;
    *exit_min = UINT32_MAX;
    portENTER_CRITICAL(&s_task_slots_lock);
    const int i = task_slot_(name, false);
    if (i >= 0)
    {
        *exit_min = s_task_slots[i].exit_min;
        if (s_task_slots[i].live != NULL)
        {
            *live = (uint32_t)uxTaskGetStackHighWaterMark(s_task_slots[i].live);
        }
    }
    portEXIT_CRITICAL(&s_task_slots_lock);
    return i >= 0;
}

struct mmosal_task_arg
{
    mmosal_task_fn_t task_fn;
    void *task_fn_arg;
};

void mmosal_task_main(void *arg)
{
    struct mmosal_task_arg task_arg = *(struct mmosal_task_arg *)arg;
    mmosal_free(arg);
    task_stack_enter_();
    task_arg.task_fn(task_arg.task_fn_arg);
    task_stack_exit_();
    mmosal_task_delete(NULL);
}

struct mmosal_task *mmosal_task_create(mmosal_task_fn_t task_fn,
                                       void *argument,
                                       enum mmosal_task_priority priority,
                                       unsigned stack_size_u32,
                                       const char *name)
{
    TaskHandle_t handle;
    UBaseType_t freertos_priority = tskIDLE_PRIORITY + priority;

    struct mmosal_task_arg *task_arg = (struct mmosal_task_arg *)mmosal_malloc(sizeof(*task_arg));
    if (task_arg == NULL)
    {
        return NULL;
    }
    task_arg->task_fn = task_fn;
    task_arg->task_fn_arg = argument;

    BaseType_t result = xTaskCreate(mmosal_task_main,
                                    name,
                                    stack_size_u32 * 4,
                                    task_arg,
                                    freertos_priority,
                                    &handle);
    if (result == pdFAIL)
    {
        mmosal_free(task_arg);
        return NULL;
    }

    return (struct mmosal_task *)handle;
}

void mmosal_task_delete(struct mmosal_task *task)
{
    vTaskDelete((TaskHandle_t)task);
}

/*
 * Warning: this function should not be used since eTaskGetState() is not a reliable
 * means of testing whether a task has completed.
 *
 * This function will be removed in future.
 */
void mmosal_task_join(struct mmosal_task *task)
{
    while (eTaskGetState((TaskHandle_t)task) != eDeleted)
    {
        mmosal_task_sleep(10);
    }
}

struct mmosal_task *mmosal_task_get_active(void)
{
    return (struct mmosal_task *)xTaskGetCurrentTaskHandle();
}

void mmosal_task_yield(void)
{
    taskYIELD();
}

void mmosal_task_sleep(uint32_t duration_ms)
{
    vTaskDelay(duration_ms / portTICK_PERIOD_MS);
}

static portMUX_TYPE task_spinlock = portMUX_INITIALIZER_UNLOCKED;

void mmosal_task_enter_critical(void)
{
    taskENTER_CRITICAL(&task_spinlock);
}

void mmosal_task_exit_critical(void)
{
    taskEXIT_CRITICAL(&task_spinlock);
}

void mmosal_disable_interrupts(void)
{
    taskDISABLE_INTERRUPTS();
}

void mmosal_enable_interrupts(void)
{
    taskENABLE_INTERRUPTS();
}

const char *mmosal_task_name(void)
{
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    return pcTaskGetName(t);
}

bool mmosal_task_wait_for_notification(uint32_t timeout_ms)
{
    TickType_t wait = portMAX_DELAY;
    if (timeout_ms < UINT32_MAX)
    {
        wait = pdMS_TO_TICKS(timeout_ms);
    }
    uint32_t ret = ulTaskNotifyTake(pdTRUE, /* Act as binary semaphore */
                                    wait);
    return (ret != 0);
}

void mmosal_task_notify(struct mmosal_task *task)
{
    xTaskNotifyGive((TaskHandle_t)task);
}

void mmosal_task_notify_from_isr(struct mmosal_task *task)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)task, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/* --------------------------------------------------------------------------------------------- */

struct mmosal_mutex *mmosal_mutex_create(const char *name)
{
    struct mmosal_mutex *mutex = (struct mmosal_mutex *)xSemaphoreCreateMutex();
#if (configUSE_TRACE_FACILITY == 1) && defined(ENABLE_TRACEALYZER) && ENABLE_TRACEALYZER
    if (name != NULL)
    {
        vTraceSetMutexName(mutex, name);
    }
#else
    (void)name;
#endif
    return mutex;
}

void mmosal_mutex_delete(struct mmosal_mutex *mutex)
{
    if (mutex != NULL)
    {
        vQueueDelete((SemaphoreHandle_t)mutex);
    }
}

bool mmosal_mutex_get(struct mmosal_mutex *mutex, uint32_t timeout_ms)
{
    uint32_t timeout_ticks = portMAX_DELAY;
    if (timeout_ms != UINT32_MAX)
    {
        timeout_ticks = timeout_ms / portTICK_PERIOD_MS;
    }
    return (xSemaphoreTake((SemaphoreHandle_t)mutex, timeout_ticks) == pdPASS);
}

bool mmosal_mutex_release(struct mmosal_mutex *mutex)
{
    return (xSemaphoreGive((SemaphoreHandle_t)mutex) == pdPASS);
}

bool mmosal_mutex_is_held_by_active_task(struct mmosal_mutex *mutex)
{
    return xSemaphoreGetMutexHolder((SemaphoreHandle_t)mutex) == xTaskGetCurrentTaskHandle();
}

/* --------------------------------------------------------------------------------------------- */

struct mmosal_sem *mmosal_sem_create(unsigned max_count, unsigned initial_count, const char *name)
{
    struct mmosal_sem *sem =
        (struct mmosal_sem *)xSemaphoreCreateCounting(max_count, initial_count);
#if (configUSE_TRACE_FACILITY == 1) && defined(ENABLE_TRACEALYZER) && ENABLE_TRACEALYZER
    if (name != NULL)
    {
        vTraceSetSemaphoreName(sem, name);
    }
#else
    (void)name;
#endif
    return sem;
}

void mmosal_sem_delete(struct mmosal_sem *sem)
{
    vQueueDelete((SemaphoreHandle_t)sem);
}

bool mmosal_sem_give(struct mmosal_sem *sem)
{
    return xSemaphoreGive((SemaphoreHandle_t)sem);
}

bool mmosal_sem_give_from_isr(struct mmosal_sem *sem)
{
    BaseType_t task_woken = false;
    BaseType_t ret = xSemaphoreGiveFromISR((SemaphoreHandle_t)sem, &task_woken);
    if (ret == pdPASS)
    {
        portYIELD_FROM_ISR(task_woken);
        return true;
    }
    else
    {
        return false;
    }
}

bool mmosal_sem_wait(struct mmosal_sem *sem, uint32_t timeout_ms)
{
    uint32_t timeout_ticks = portMAX_DELAY;
    if (timeout_ms != UINT32_MAX)
    {
        timeout_ticks = timeout_ms / portTICK_PERIOD_MS;
    }
    return (xSemaphoreTake((SemaphoreHandle_t)sem, timeout_ticks) == pdPASS);
}

uint32_t mmosal_sem_get_count(struct mmosal_sem *sem)
{
    return uxSemaphoreGetCount((SemaphoreHandle_t)sem);
}

/* --------------------------------------------------------------------------------------------- */

struct mmosal_semb *mmosal_semb_create(const char *name)
{
    struct mmosal_semb *semb = (struct mmosal_semb *)xSemaphoreCreateBinary();
#if (configUSE_TRACE_FACILITY == 1) && defined(ENABLE_TRACEALYZER) && ENABLE_TRACEALYZER
    if (name != NULL)
    {
        vTraceSetSemaphoreName(semb, name);
    }
#else
    (void)name;
#endif
    return semb;
}

void mmosal_semb_delete(struct mmosal_semb *semb)
{
    vQueueDelete((SemaphoreHandle_t)semb);
}

bool mmosal_semb_give(struct mmosal_semb *semb)
{
    return (xSemaphoreGive((SemaphoreHandle_t)semb) == pdPASS);
}

bool mmosal_semb_give_from_isr(struct mmosal_semb *semb)
{
    BaseType_t task_woken = pdFALSE;
    BaseType_t ret = xSemaphoreGiveFromISR((SemaphoreHandle_t)semb, &task_woken);
    if (ret == pdPASS)
    {
        portYIELD_FROM_ISR(task_woken);
        return true;
    }
    else
    {
        return false;
    }
}

bool mmosal_semb_wait(struct mmosal_semb *semb, uint32_t timeout_ms)
{
    uint32_t timeout_ticks = portMAX_DELAY;
    if (timeout_ms != UINT32_MAX)
    {
        timeout_ticks = timeout_ms / portTICK_PERIOD_MS;
    }
    return (xSemaphoreTake((SemaphoreHandle_t)semb, timeout_ticks) == pdPASS);
}

/* --------------------------------------------------------------------------------------------- */

struct mmosal_queue *mmosal_queue_create(size_t num_items, size_t item_size, const char *name)
{
    struct mmosal_queue *queue = (struct mmosal_queue *)xQueueCreate(num_items, item_size);
#if (configUSE_TRACE_FACILITY == 1) && defined(ENABLE_TRACEALYZER) && ENABLE_TRACEALYZER
    if (name != NULL)
    {
        vTraceSetQueueName(queue, name);
    }
#else
    (void)name;
#endif
    return queue;
}

void mmosal_queue_delete(struct mmosal_queue *queue)
{
    vQueueDelete((SemaphoreHandle_t)queue);
}

bool mmosal_queue_pop(struct mmosal_queue *queue, void *item, uint32_t timeout_ms)
{
    uint32_t timeout_ticks = portMAX_DELAY;
    if (timeout_ms != UINT32_MAX)
    {
        timeout_ticks = timeout_ms / portTICK_PERIOD_MS;
    }
    return (xQueueReceive((SemaphoreHandle_t)queue, item, timeout_ticks) == pdPASS);
}

bool mmosal_queue_push(struct mmosal_queue *queue, const void *item, uint32_t timeout_ms)
{
    uint32_t timeout_ticks = portMAX_DELAY;
    if (timeout_ms != UINT32_MAX)
    {
        timeout_ticks = timeout_ms / portTICK_PERIOD_MS;
    }
    return (xQueueSendToBack((SemaphoreHandle_t)queue, item, timeout_ticks) == pdPASS);
}

bool mmosal_queue_pop_from_isr(struct mmosal_queue *queue, void *item)
{
    BaseType_t task_woken = pdFALSE;
    if (xQueueReceiveFromISR((SemaphoreHandle_t)queue, item, &task_woken) == pdTRUE)
    {
        portYIELD_FROM_ISR(task_woken);
        return true;
    }
    else
    {
        return false;
    }
}

bool mmosal_queue_push_from_isr(struct mmosal_queue *queue, const void *item)
{
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendToBackFromISR((SemaphoreHandle_t)queue, item, &task_woken) == pdTRUE)
    {
        portYIELD_FROM_ISR(task_woken);
        return true;
    }
    else
    {
        return false;
    }
}

/* --------------------------------------------------------------------------------------------- */

uint32_t mmosal_get_time_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

uint32_t mmosal_get_time_ticks(void)
{
    return xTaskGetTickCount();
}

uint32_t mmosal_ticks_per_second(void)
{
    return portTICK_PERIOD_MS * 1000;
}

/* --------------------------------------------------------------------------------------------- */

/**
 * @struct mmosal_timer
 * @brief Structure representing a timer in the MMOSAL (Morse Micro OS Abstraction Layer)
 *
 * This structure encapsulates the necessary information for managing an ESP timer.
 */
struct mmosal_timer
{
    esp_timer_handle_t handle; /**< ESP timer handle. */
    void *arg; /**< User-provided argument to be passed to the callback. */
    timer_callback_t callback; /**< Function to be called when the timer expires. */
    bool auto_reload; /**< If true, the timer will auto restart after expiring. */
    uint64_t period_us; /**< Timer period in microseconds. */
};

static void internal_timer_callback(void *arg)
{
    struct mmosal_timer *timer = (struct mmosal_timer *)arg;
    if (timer && timer->callback)
    {
        timer->callback(timer);
    }
}

struct mmosal_timer *mmosal_timer_create(const char *name,
                                         uint32_t timer_period_ms,
                                         bool auto_reload,
                                         void *arg,
                                         timer_callback_t callback)
{
    esp_timer_create_args_t timer_args = {
        .callback = internal_timer_callback,
        .arg = NULL,
        .name = name,
        .skip_unhandled_events = true,
        .dispatch_method = ESP_TIMER_TASK,
    };

    struct mmosal_timer *timer = mmosal_malloc(sizeof(struct mmosal_timer));
    if (timer == NULL)
    {
        return NULL;
    }

    timer->arg = arg;
    timer->callback = callback;
    timer->auto_reload = auto_reload;
    timer->period_us = timer_period_ms * 1000ULL;
    timer_args.arg = timer;

    if (esp_timer_create(&timer_args, &timer->handle) != ESP_OK)
    {
        mmosal_free(timer);
        return NULL;
    }

    return timer;
}

void mmosal_timer_delete(struct mmosal_timer *timer)
{
    if (timer != NULL)
    {
        esp_timer_stop(timer->handle);
        esp_timer_delete(timer->handle);
        mmosal_free(timer);
    }
}

bool mmosal_timer_start(struct mmosal_timer *timer)
{
    MMOSAL_DEV_ASSERT(timer);

    if (timer->auto_reload)
    {
        return esp_timer_start_periodic(timer->handle, timer->period_us) == ESP_OK;
    }
    else
    {
        return esp_timer_start_once(timer->handle, timer->period_us) == ESP_OK;
    }
}

bool mmosal_timer_stop(struct mmosal_timer *timer)
{
    MMOSAL_DEV_ASSERT(timer);

    return esp_timer_stop(timer->handle) == ESP_OK;
}

bool mmosal_timer_change_period(struct mmosal_timer *timer, uint32_t new_period_ms)
{
    MMOSAL_DEV_ASSERT(timer);

    timer->period_us = new_period_ms * 1000ULL;
    return esp_timer_restart(timer->handle, timer->period_us) == ESP_OK;
}

void *mmosal_timer_get_arg(struct mmosal_timer *timer)
{
    MMOSAL_DEV_ASSERT(timer);

    return timer->arg;
}

bool mmosal_is_timer_active(struct mmosal_timer *timer)
{
    MMOSAL_DEV_ASSERT(timer);

    return esp_timer_is_active(timer->handle);
}

/* --------------------------------------------------------------------------------------------- */

int mmosal_printf(const char *format, ...)
{
    int ret;
    va_list args;
    va_start(args, format);
    ret = vprintf(format, args);
    va_end(args);
    return ret;
}
