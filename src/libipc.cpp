/**
 * @file libipc.cpp
 * @brief Implementation of the IPC communication library (libipc.so).
 */
#include "libipc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Internal state (per-process) --- */

static SharedMemoryLayout *g_shm = nullptr;
static sem_t *g_mutex_sem = nullptr;
static sem_t *g_server_sem = nullptr;
static sem_t *g_slot_sems[IPC_MAX_SLOTS] = {};
static int    g_shm_fd = -1;
static uint64_t g_known_generation = 0;

/* --- Helper: build slot semaphore name --- */

static void slot_sem_name(int index, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s%d", IPC_SLOT_SEM_PREFIX, index);
}

static int sem_wait_with_timeout(sem_t *sem, int timeout_sec)
{
    while (true) {
        timespec ts{};
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return -1;
        ts.tv_sec += timeout_sec;

        if (sem_timedwait(sem, &ts) == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static bool shm_object_replaced()
{
    if (g_shm_fd < 0)
        return false;

    int live_fd = shm_open(IPC_SHM_NAME, O_RDWR, 0666);
    if (live_fd < 0)
        return false;

    struct stat cur{};
    struct stat live{};
    bool replaced = false;
    if (fstat(g_shm_fd, &cur) == 0 && fstat(live_fd, &live) == 0) {
        replaced = (cur.st_dev != live.st_dev) || (cur.st_ino != live.st_ino);
    }
    close(live_fd);
    return replaced;
}

static int reconnect_after_server_restart()
{
    ipc_cleanup();
    if (ipc_init() == 0)
        return IPC_ERR_SERVER_RESTARTED;
    return -1;
}

static int ensure_fresh_connection()
{
    if (!g_shm)
        return -1;

    if (shm_object_replaced())
        return reconnect_after_server_restart();

    if (g_shm->server_generation != g_known_generation)
        return reconnect_after_server_restart();

    return 0;
}

static int lock_shared_mutex_with_recovery()
{
    static constexpr int kMaxMutexTimeoutRetries = 5;
    int retries = 0;
    while (retries < kMaxMutexTimeoutRetries) {
        if (sem_wait_with_timeout(g_mutex_sem, 1) == 0)
            return 0;
        if (errno == ETIMEDOUT) {
            int rc = ensure_fresh_connection();
            if (rc != 0)
                return rc;
            ++retries;
            continue;
        }
        return -1;
    }
    // Prevent indefinite hangs if semaphore stays stale/blocked.
    return reconnect_after_server_restart();
}

/* --- Public API (extern "C") --- */

extern "C" int ipc_init(void)
{
    g_shm_fd = shm_open(IPC_SHM_NAME, O_RDWR, 0666);
    if (g_shm_fd < 0) {
        perror("ipc_init: shm_open");
        return -1;
    }

    g_shm = static_cast<SharedMemoryLayout *>(
        mmap(nullptr, sizeof(SharedMemoryLayout),
             PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0));
    if (g_shm == MAP_FAILED) {
        perror("ipc_init: mmap");
        close(g_shm_fd);
        g_shm_fd = -1;
        g_shm = nullptr;
        return -1;
    }

    g_mutex_sem = sem_open(IPC_MUTEX_NAME, 0);
    if (g_mutex_sem == SEM_FAILED) {
        perror("ipc_init: sem_open mutex");
        goto fail;
    }

    g_server_sem = sem_open(IPC_SERVER_SEM_NAME, 0);
    if (g_server_sem == SEM_FAILED) {
        perror("ipc_init: sem_open server_notify");
        goto fail;
    }

    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        char name[64];
        slot_sem_name(i, name, sizeof(name));
        g_slot_sems[i] = sem_open(name, 0);
        if (g_slot_sems[i] == SEM_FAILED) {
            perror("ipc_init: sem_open slot");
            goto fail;
        }
    }

    g_known_generation = g_shm->server_generation;
    return 0;

fail:
    ipc_cleanup();
    return -1;
}

extern "C" void ipc_cleanup(void)
{
    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        if (g_slot_sems[i] && g_slot_sems[i] != SEM_FAILED) {
            sem_close(g_slot_sems[i]);
            g_slot_sems[i] = nullptr;
        }
    }
    if (g_server_sem && g_server_sem != SEM_FAILED) {
        sem_close(g_server_sem);
        g_server_sem = nullptr;
    }
    if (g_mutex_sem && g_mutex_sem != SEM_FAILED) {
        sem_close(g_mutex_sem);
        g_mutex_sem = nullptr;
    }
    if (g_shm && g_shm != MAP_FAILED) {
        munmap(g_shm, sizeof(SharedMemoryLayout));
        g_shm = nullptr;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    g_known_generation = 0;
}

/* --- Internal helpers --- */

static int find_free_slot(void)
{
    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        if (g_shm->slots[i].state == IPC_SLOT_FREE)
            return i;
    }
    return -1;
}

static int validate_string(const char *s)
{
    if (!s) return -1;
    size_t len = strlen(s);
    if (len < 1 || len > IPC_MAX_STRING_LEN) return -1;
    return 0;
}

static int submit_request(ipc_cmd_t cmd, const RequestPayload *payload,
                          int *out_slot, uint64_t *out_id)
{
    int rc = ensure_fresh_connection();
    if (rc != 0)
        return rc;

    rc = lock_shared_mutex_with_recovery();
    if (rc != 0)
        return rc;

    if (g_shm->server_generation != g_known_generation) {
        sem_post(g_mutex_sem);
        return reconnect_after_server_restart();
    }

    int idx = find_free_slot();
    if (idx < 0) {
        sem_post(g_mutex_sem);
        fprintf(stderr, "submit_request: no free slots\n");
        return -1;
    }

    MessageSlot *slot = &g_shm->slots[idx];
    slot->request_id = g_shm->next_request_id++;
    slot->client_pid = getpid();
    slot->command    = cmd;
    slot->request    = *payload;
    slot->state      = IPC_SLOT_REQUEST_PENDING;

    if (out_slot) *out_slot = idx;
    if (out_id)   *out_id = slot->request_id;

    sem_post(g_mutex_sem);
    sem_post(g_server_sem);
    return 0;
}

/* --- Blocking calls --- */

static int blocking_math(ipc_cmd_t cmd, int32_t a, int32_t b, int32_t *result)
{
    if (!result) return -1;

    RequestPayload payload;
    payload.math.a = a;
    payload.math.b = b;

    int slot_idx = -1;
    uint64_t expected_request_id = 0;
    int submit_rc = submit_request(cmd, &payload, &slot_idx, &expected_request_id);
    if (submit_rc != 0)
        return submit_rc;
    // Blocking calls are completed via per-slot semaphores. Validate that the slot
    // truly contains this request's response to guard against stale semaphore wakeups.
    static constexpr int kMaxSlotWaitTimeoutRetries = 16;
    int retries = 0;
    while (retries < kMaxSlotWaitTimeoutRetries) {
        if (sem_wait_with_timeout(g_slot_sems[slot_idx], 1) == 0) {
            int rc = lock_shared_mutex_with_recovery();
            if (rc != 0)
                return rc;

            MessageSlot *slot = &g_shm->slots[slot_idx];
            if (slot->request_id == expected_request_id &&
                slot->state == IPC_SLOT_RESPONSE_READY) {
                *result = slot->response.math_result;
                int ret = (slot->status == IPC_STATUS_OK) ? 0 : -1;
                slot->state = IPC_SLOT_FREE;
                sem_post(g_mutex_sem);
                return ret;
            }

            sem_post(g_mutex_sem);
            ++retries;
            continue;
        }
        if (errno == ETIMEDOUT) {
            int rc = ensure_fresh_connection();
            if (rc != 0)
                return rc;
            ++retries;
            continue;
        }
        return -1;
    }
    if (retries >= kMaxSlotWaitTimeoutRetries)
        return reconnect_after_server_restart();
    return -1;
}

extern "C" int ipc_add(int32_t a, int32_t b, int32_t *result)
{
    return blocking_math(IPC_CMD_ADD, a, b, result);
}

extern "C" int ipc_subtract(int32_t a, int32_t b, int32_t *result)
{
    return blocking_math(IPC_CMD_SUB, a, b, result);
}

/* --- Non-blocking calls --- */

static int async_math(ipc_cmd_t cmd, int32_t a, int32_t b,
                      uint64_t *request_id)
{
    if (!request_id) return -1;

    RequestPayload payload;
    payload.math.a = a;
    payload.math.b = b;

    return submit_request(cmd, &payload, nullptr, request_id);
}

static int async_string(ipc_cmd_t cmd, const char *s1, const char *s2,
                        uint64_t *request_id)
{
    if (!request_id) return -1;
    if (validate_string(s1) != 0 || validate_string(s2) != 0) {
        fprintf(stderr, "async_string: invalid string length "
                "(must be 1..%d chars)\n", IPC_MAX_STRING_LEN);
        return -1;
    }

    RequestPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.str.s1, s1, IPC_MAX_STRING_LEN);
    payload.str.s1[IPC_MAX_STRING_LEN] = '\0';
    strncpy(payload.str.s2, s2, IPC_MAX_STRING_LEN);
    payload.str.s2[IPC_MAX_STRING_LEN] = '\0';

    return submit_request(cmd, &payload, nullptr, request_id);
}

extern "C" int ipc_multiply(int32_t a, int32_t b, uint64_t *request_id)
{
    return async_math(IPC_CMD_MUL, a, b, request_id);
}

extern "C" int ipc_divide(int32_t a, int32_t b, uint64_t *request_id)
{
    return async_math(IPC_CMD_DIV, a, b, request_id);
}

extern "C" int ipc_concat(const char *s1, const char *s2,
                           uint64_t *request_id)
{
    return async_string(IPC_CMD_CONCAT, s1, s2, request_id);
}

extern "C" int ipc_search(const char *haystack, const char *needle,
                           uint64_t *request_id)
{
    return async_string(IPC_CMD_SEARCH, haystack, needle, request_id);
}

extern "C" int ipc_get_result(uint64_t request_id, ResponsePayload *result,
                               ipc_status_t *status)
{
    if (!result || !status) return -1;

    int rc = ensure_fresh_connection();
    if (rc != 0)
        return rc;

    rc = lock_shared_mutex_with_recovery();
    if (rc != 0)
        return rc;

    if (g_shm->server_generation != g_known_generation) {
        sem_post(g_mutex_sem);
        return reconnect_after_server_restart();
    }

    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        MessageSlot *slot = &g_shm->slots[i];
        if (slot->request_id == request_id) {
            if (slot->state == IPC_SLOT_RESPONSE_READY) {
                *result = slot->response;
                *status = slot->status;
                slot->state = IPC_SLOT_FREE;
                sem_post(g_mutex_sem);
                return 0;
            }
            sem_post(g_mutex_sem);
            return IPC_NOT_READY;
        }
    }

    sem_post(g_mutex_sem);
    return -1;
}
