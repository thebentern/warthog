/*
 * The SPI transport's start and its unwind (driver/transport/sdio.c), compiled out of the SDK and
 * run on stubs: the spi_irq globals, morse_spi_irq_main, morse_trns_start_unwind and
 * morse_trns_start, extracted by name (the Makefile writes test_sdio_start.fns). Every start here
 * fails after its spi_irq task exists (the CCCR write fails), as with a chip that does not answer;
 * the task never runs, but at the unwind's first 10 ms sleep it sees spi_irq_task_run cleared and
 * stops when the case lets it.
 *  (1) a task that stops in time: the unwind frees the bus lock and the IRQ semaphore and calls
 *      mmhal_wlan_deinit, and the next start runs from scratch;
 *  (2) a task starved past the unwind's 500 ms wait: the lock and semaphore are leaked to it, not
 *      freed under it, and mmhal_wlan_deinit still runs; the next start fails at once, making no
 *      second lock, semaphore or task and not calling mmhal_wlan_init (RESET_N stays low), so
 *      mmhalow_init's later attempts end radio-down.
 */
#define MMLOG_LEVEL_OVRD 5 /* as sdio.c */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "morse_transport.h"
#include "driver/morse_driver/hw.h"
#include "driver/driver.h"
#include "mmosal.h"
#include "mmhal_wlan.h"
#include "mmlog.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else      { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* ---- what the extracted functions reach --------------------------------- */

#define SPI_IRQ_TASK_PRIORITY (MMOSAL_TASK_PRI_HIGH)
#define SPI_IRQ_TASK_STACK    (1024)
#define TRANSPORT_FSM_TRACE_INIT() do { } while (0)
#define ADDR_BASE_FSM_TRACE_INIT() do { } while (0)
#define SDIO_CCCR_IEN_ADDR 0x04u
#define SDIO_CCCR_IEN_IENM (1u)
#define SDIO_CCCR_IEN_IEN1 (1u << 1)
#define SDIO_CCCR_BIC_ADDR 0x07u
#define SDIO_CCCR_BIC_ECSI (1u << 5)

struct mmosal_mutex { int unused; };
struct mmosal_semb { int unused; };
struct mmosal_task { int unused; };
static struct mmosal_mutex s_mutex[4];
static struct mmosal_semb s_semb[4];
static struct mmosal_task s_task[4];
static unsigned s_mutexes, s_mutex_dels, s_sembs, s_semb_dels, s_tasks, s_hal_inits, s_hal_deinits, s_asserts;
static bool s_task_stops; /* the spi_irq task exits once told to */

void mmosal_log_failure_info(const struct mmosal_failure_info *info) { (void)info; }
void mmosal_impl_assert(void) { s_asserts++; }
int mmosal_printf(const char *format, ...) { (void)format; return 0; }
const char *mmosal_task_name(void) { return "test"; }
uint32_t mmosal_get_time_ms(void) { return 0; }
struct mmosal_mutex *mmosal_mutex_create(const char *name) { (void)name; return &s_mutex[s_mutexes++ % 4u]; }
void mmosal_mutex_delete(struct mmosal_mutex *mutex) { (void)mutex; s_mutex_dels++; }
struct mmosal_semb *mmosal_semb_create(const char *name) { (void)name; return &s_semb[s_sembs++ % 4u]; }
void mmosal_semb_delete(struct mmosal_semb *semb) { (void)semb; s_semb_dels++; }
bool mmosal_semb_give(struct mmosal_semb *semb) { (void)semb; return true; }
bool mmosal_semb_wait(struct mmosal_semb *semb, uint32_t timeout_ms) { (void)semb; (void)timeout_ms; return true; }
struct mmosal_task *mmosal_task_create(mmosal_task_fn_t task_fn, void *argument, enum mmosal_task_priority priority,
                                       unsigned stack_size_u32, const char *name)
{
    (void)task_fn; (void)argument; (void)priority; (void)stack_size_u32; (void)name;
    return &s_task[s_tasks++ % 4u];
}
void mmhal_wlan_init(void) { s_hal_inits++; }
void mmhal_wlan_deinit(void) { s_hal_deinits++; }
void mmhal_wlan_register_spi_irq_handler(mmhal_irq_handler_t handler) { (void)handler; }
void morse_trns_claim(struct driver_data *driverd) { (void)driverd; }
void morse_trns_release(struct driver_data *driverd) { (void)driverd; }
int morse_hw_irq_handle(struct driver_data *driverd) { (void)driverd; return 0; }
static morse_error_t morse_trns_reset(struct driver_data *driverd) { (void)driverd; return MORSE_SUCCESS; }
static morse_error_t morse_cmd52_write(uint32_t address, uint8_t data, enum mmhal_sdio_function function)
{
    (void)address; (void)data; (void)function;
    return MORSE_FAILED;
}
static morse_error_t morse_cmd52_read(uint32_t address, uint8_t *data, enum mmhal_sdio_function function)
{
    (void)address; (void)data; (void)function;
    return MORSE_FAILED;
}
static void morse_spi_irq_handler(void) { }

#include "test_sdio_start.fns"

/* The unwind's wait: the spi_irq task runs while it sleeps. */
void mmosal_task_sleep(uint32_t duration_ms)
{
    (void)duration_ms;
    if (s_task_stops && !spi_irq_task_run) { spi_irq_task_has_finished = true; }
}

static void t_task_stops(void)
{
    struct driver_data d;
    s_task_stops = true;
    const morse_error_t r1 = morse_trns_start(&d);
    CHECK(r1 == MORSE_FAILED && s_tasks == 1u && s_mutex_dels == 1u && s_semb_dels == 1u && s_hal_deinits == 1u &&
              bus_lock == NULL && spi_irq_semb == NULL,
          "(1) a task that stops: the failed start frees the lock and semaphore and calls mmhal_wlan_deinit "
          "(freed %u/%u, deinits %u)", s_mutex_dels, s_semb_dels, s_hal_deinits);
    const morse_error_t r2 = morse_trns_start(&d);
    CHECK(r2 == MORSE_FAILED && s_mutexes == 2u && s_sembs == 2u && s_tasks == 2u && s_hal_inits == 2u &&
              bus_lock == NULL,
          "(1) and the next start runs from scratch (locks %u, semaphores %u, tasks %u, hal inits %u)", s_mutexes,
          s_sembs, s_tasks, s_hal_inits);
}

static void t_task_starved(void)
{
    struct driver_data d;
    s_task_stops = false;
    s_mutexes = s_mutex_dels = s_sembs = s_semb_dels = s_tasks = s_hal_inits = s_hal_deinits = 0;
    const morse_error_t r0 = morse_trns_start(&d);
    CHECK(r0 == MORSE_FAILED && s_tasks == 1u && s_mutex_dels == 0u && s_semb_dels == 0u && s_hal_deinits == 1u &&
              bus_lock != NULL && spi_irq_semb != NULL && !spi_irq_task_run,
          "(2) a starved task: the failed start leaks the lock and semaphore to it, told to stop, and calls "
          "mmhal_wlan_deinit (freed %u/%u, deinits %u)", s_mutex_dels, s_semb_dels, s_hal_deinits);
    const struct mmosal_mutex *lock = bus_lock;
    const struct mmosal_semb *semb = spi_irq_semb;
    const morse_error_t r = morse_trns_start(&d);
    CHECK(r == MORSE_FAILED, "(2) the next start fails (%d)", (int)r);
    CHECK(s_mutexes == 1u && s_sembs == 1u && bus_lock == lock && spi_irq_semb == semb,
          "(2) and makes no second lock or semaphore over the stale task's (locks %u, semaphores %u)", s_mutexes,
          s_sembs);
    CHECK(s_tasks == 1u, "(2) and starts no second spi_irq task beside the stale one (tasks %u)", s_tasks);
    CHECK(s_hal_inits == 1u, "(2) and does not call mmhal_wlan_init again: RESET_N stays low (%u)", s_hal_inits);
}

int main(void)
{
    printf("=== the SPI transport's start and unwind (sdio.c) ===\n");
    t_task_stops();
    t_task_starved();
    CHECK(s_asserts == 0u, "no assert fired (%u)", s_asserts);
    if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
    printf("test_sdio_start: all passed\n");
    return 0;
}
