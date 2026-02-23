/**
 * @file server.cpp
 * @brief IPC server: creates shared memory, dispatches requests to thread pools.
 */
#include "ipc_defs.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <queue>
#include <semaphore.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

/* ================================================================== */
/*  ShutdownMode                                                       */
/* ================================================================== */

enum class ShutdownMode { Drain, Immediate };

/* ================================================================== */
/*  ThreadPool -- simplified C++17 pool based on CPP11_ThreadPool      */
/* ================================================================== */

class ThreadPool {
public:
    ThreadPool(size_t num_threads, std::function<void(int)> handler)
        : task_handler_(std::move(handler))
    {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    int slot_index;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] {
                            return stop_.load() || !queue_.empty();
                        });
                        if (stop_.load() && queue_.empty())
                            return;
                        slot_index = queue_.front();
                        queue_.pop();
                    }
                    task_handler_(slot_index);
                }
            });
        }
    }

    ~ThreadPool() { shutdown(); }

    bool submit(int slot_index)
    {
        {
            std::scoped_lock lock(mutex_);
            if (stop_.load())
                return false;
            queue_.push(slot_index);
        }
        cv_.notify_one();
        return true;
    }

    size_t shutdown(ShutdownMode mode = ShutdownMode::Drain)
    {
        size_t discarded = 0;
        if (stop_.exchange(true))
            return 0;
        if (mode == ShutdownMode::Immediate) {
            std::scoped_lock lock(mutex_);
            discarded = queue_.size();
            std::queue<int> empty;
            queue_.swap(empty);
        }
        cv_.notify_all();
        for (auto &w : workers_) {
            if (w.joinable())
                w.join();
        }
        return discarded;
    }

    size_t pending_count() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

private:
    std::vector<std::thread>     workers_;
    std::queue<int>              queue_;
    mutable std::mutex           mutex_;
    std::condition_variable      cv_;
    std::atomic<bool>            stop_{false};
    std::function<void(int)>     task_handler_;
};

/* ================================================================== */
/*  Global state                                                       */
/* ================================================================== */

static const char *LOCK_FILE = "/tmp/ipc_server.lock";
static const char *GENERATION_FILE = "/tmp/ipc_server.generation";

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_status_requested{false};
static ShutdownMode g_shutdown_mode = ShutdownMode::Drain;
static int g_lock_fd = -1;
static SharedMemoryLayout *g_shm = nullptr;
static int g_shm_fd = -1;
static sem_t *g_mutex_sem = nullptr;
static sem_t *g_server_sem = nullptr;
static sem_t *g_slot_sems[IPC_MAX_SLOTS] = {};

static uint64_t next_server_generation()
{
    int fd = open(GENERATION_FILE, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return static_cast<uint64_t>(time(nullptr));
    }

    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return static_cast<uint64_t>(time(nullptr));
    }

    uint64_t gen = 0;
    ssize_t n = read(fd, &gen, sizeof(gen));
    if (n != static_cast<ssize_t>(sizeof(gen))) {
        gen = 0;
    }
    ++gen;

    lseek(fd, 0, SEEK_SET);
    ssize_t written = write(fd, &gen, sizeof(gen));
    if (written == static_cast<ssize_t>(sizeof(gen))) {
        ftruncate(fd, sizeof(gen));
    }

    flock(fd, LOCK_UN);
    close(fd);
    return gen;
}

static size_t default_threads_per_pool()
{
    unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 2)
        return 1;
    return (hw - 1) / 2;
}

static void slot_sem_name(int index, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s%d", IPC_SLOT_SEM_PREFIX, index);
}

/* ================================================================== */
/*  Signal handler                                                     */
/* ================================================================== */

static void signal_handler(int /*sig*/)
{
    g_running.store(false);
    if (g_server_sem)
        sem_post(g_server_sem);
}

static void status_handler(int /*sig*/)
{
    g_status_requested.store(true);
    if (g_server_sem)
        sem_post(g_server_sem);
}

/* ================================================================== */
/*  Worker functions                                                   */
/* ================================================================== */

static void process_math(int slot_idx)
{
    sem_wait(g_mutex_sem);
    MessageSlot *slot = &g_shm->slots[slot_idx];
    ipc_cmd_t cmd = slot->command;
    int32_t a = slot->request.math.a;
    int32_t b = slot->request.math.b;
    sem_post(g_mutex_sem);

    if (cmd == IPC_CMD_MUL || cmd == IPC_CMD_DIV)
        std::this_thread::sleep_for(std::chrono::seconds(2));

    int32_t result = 0;
    ipc_status_t status = IPC_STATUS_OK;

    switch (cmd) {
    case IPC_CMD_ADD: result = a + b; break;
    case IPC_CMD_SUB: result = a - b; break;
    case IPC_CMD_MUL: result = a * b; break;
    case IPC_CMD_DIV:
        if (b == 0) {
            status = IPC_STATUS_DIV_BY_ZERO;
        } else {
            result = a / b;
        }
        break;
    default:
        status = IPC_STATUS_INVALID_INPUT;
        break;
    }

    sem_wait(g_mutex_sem);
    slot->response.math_result = result;
    slot->status = status;
    slot->state = IPC_SLOT_RESPONSE_READY;
    sem_post(g_mutex_sem);

    sem_post(g_slot_sems[slot_idx]);
}

static void process_string(int slot_idx)
{
    sem_wait(g_mutex_sem);
    MessageSlot *slot = &g_shm->slots[slot_idx];
    ipc_cmd_t cmd = slot->command;
    char s1[IPC_MAX_STRING_LEN + 1];
    char s2[IPC_MAX_STRING_LEN + 1];
    strncpy(s1, slot->request.str.s1, IPC_MAX_STRING_LEN + 1);
    strncpy(s2, slot->request.str.s2, IPC_MAX_STRING_LEN + 1);
    s1[IPC_MAX_STRING_LEN] = '\0';
    s2[IPC_MAX_STRING_LEN] = '\0';
    sem_post(g_mutex_sem);

    ipc_status_t status = IPC_STATUS_OK;
    ResponsePayload resp;
    memset(&resp, 0, sizeof(resp));

    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    if (len1 < 1 || len1 > IPC_MAX_STRING_LEN ||
        len2 < 1 || len2 > IPC_MAX_STRING_LEN) {
        status = IPC_STATUS_STR_TOO_LONG;
    } else if (cmd == IPC_CMD_CONCAT) {
        if (len1 + len2 > IPC_MAX_RESULT_LEN - 1) {
            status = IPC_STATUS_STR_TOO_LONG;
        } else {
            snprintf(resp.str_result, IPC_MAX_RESULT_LEN, "%s%s", s1, s2);
        }
    } else if (cmd == IPC_CMD_SEARCH) {
        const char *pos = strstr(s1, s2);
        if (pos) {
            resp.position = static_cast<int32_t>(pos - s1);
        } else {
            resp.position = -1;
            status = IPC_STATUS_NOT_FOUND;
        }
    } else {
        status = IPC_STATUS_INVALID_INPUT;
    }

    sem_wait(g_mutex_sem);
    slot->response = resp;
    slot->status = status;
    slot->state = IPC_SLOT_RESPONSE_READY;
    sem_post(g_mutex_sem);

    sem_post(g_slot_sems[slot_idx]);
}

/* ================================================================== */
/*  Cleanup                                                            */
/* ================================================================== */

static void cleanup_ipc(void)
{
    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        if (g_slot_sems[i] && g_slot_sems[i] != SEM_FAILED) {
            sem_close(g_slot_sems[i]);
            char name[64];
            slot_sem_name(i, name, sizeof(name));
            sem_unlink(name);
        }
    }
    if (g_server_sem && g_server_sem != SEM_FAILED) {
        sem_close(g_server_sem);
        sem_unlink(IPC_SERVER_SEM_NAME);
    }
    if (g_mutex_sem && g_mutex_sem != SEM_FAILED) {
        sem_close(g_mutex_sem);
        sem_unlink(IPC_MUTEX_NAME);
    }
    if (g_shm && g_shm != MAP_FAILED) {
        munmap(g_shm, sizeof(SharedMemoryLayout));
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
    }
    shm_unlink(IPC_SHM_NAME);
    if (g_lock_fd >= 0) {
        unlink(LOCK_FILE);
        close(g_lock_fd);
    }
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

int main(int argc, const char *argv[])
{
    /* --- Parse command-line flags --- */
    size_t threads_per_pool = default_threads_per_pool();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            int val = atoi(argv[++i]);
            if (val > 0)
                threads_per_pool = static_cast<size_t>(val);
        } else if (strncmp(argv[i], "--shutdown=", 11) == 0) {
            const char *mode = argv[i] + 11;
            if (strcmp(mode, "immediate") == 0)
                g_shutdown_mode = ShutdownMode::Immediate;
            else if (strcmp(mode, "drain") == 0)
                g_shutdown_mode = ShutdownMode::Drain;
            else {
                fprintf(stderr, "Unknown shutdown mode: %s (use drain or immediate)\n", mode);
                return 1;
            }
        }
    }

    /* --- Acquire instance lock --- */
    g_lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) {
        perror("server: open lock file");
        return 1;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            fprintf(stderr,
                    "Error: another server instance is already running.\n"
                    "If the previous server crashed, remove %s and retry.\n",
                    LOCK_FILE);
        } else {
            perror("server: flock");
        }
        close(g_lock_fd);
        return 1;
    }

    /* --- Create shared memory --- */
    g_shm_fd = shm_open(IPC_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0) {
        perror("server: shm_open");
        return 1;
    }
    if (ftruncate(g_shm_fd, sizeof(SharedMemoryLayout)) < 0) {
        perror("server: ftruncate");
        close(g_shm_fd);
        shm_unlink(IPC_SHM_NAME);
        return 1;
    }
    g_shm = static_cast<SharedMemoryLayout *>(
        mmap(nullptr, sizeof(SharedMemoryLayout),
             PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0));
    if (g_shm == MAP_FAILED) {
        perror("server: mmap");
        close(g_shm_fd);
        shm_unlink(IPC_SHM_NAME);
        return 1;
    }

    uint64_t server_generation = next_server_generation();
    memset(g_shm, 0, sizeof(SharedMemoryLayout));
    g_shm->server_generation = server_generation;
    g_shm->next_request_id = 1;

    /* --- Create semaphores --- */
    g_mutex_sem = sem_open(IPC_MUTEX_NAME, O_CREAT | O_EXCL, 0666, 1);
    if (g_mutex_sem == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink(IPC_MUTEX_NAME);
            g_mutex_sem = sem_open(IPC_MUTEX_NAME, O_CREAT | O_EXCL, 0666, 1);
        }
        if (g_mutex_sem == SEM_FAILED) {
            perror("server: sem_open mutex");
            cleanup_ipc();
            return 1;
        }
    }

    g_server_sem = sem_open(IPC_SERVER_SEM_NAME, O_CREAT | O_EXCL, 0666, 0);
    if (g_server_sem == SEM_FAILED) {
        if (errno == EEXIST) {
            sem_unlink(IPC_SERVER_SEM_NAME);
            g_server_sem = sem_open(IPC_SERVER_SEM_NAME, O_CREAT | O_EXCL, 0666, 0);
        }
        if (g_server_sem == SEM_FAILED) {
            perror("server: sem_open server_notify");
            cleanup_ipc();
            return 1;
        }
    }

    for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
        char name[64];
        slot_sem_name(i, name, sizeof(name));
        g_slot_sems[i] = sem_open(name, O_CREAT | O_EXCL, 0666, 0);
        if (g_slot_sems[i] == SEM_FAILED) {
            if (errno == EEXIST) {
                sem_unlink(name);
                g_slot_sems[i] = sem_open(name, O_CREAT | O_EXCL, 0666, 0);
            }
            if (g_slot_sems[i] == SEM_FAILED) {
                perror("server: sem_open slot");
                cleanup_ipc();
                return 1;
            }
        }
    }

    /* --- Signal handling --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    struct sigaction sa_status;
    memset(&sa_status, 0, sizeof(sa_status));
    sa_status.sa_handler = status_handler;
    sigemptyset(&sa_status.sa_mask);
    sa_status.sa_flags = 0;
    sigaction(SIGUSR1, &sa_status, nullptr);

    time_t start_time = time(nullptr);

    /* --- Thread pools --- */
    ThreadPool math_pool(threads_per_pool, process_math);
    ThreadPool string_pool(threads_per_pool, process_string);

    printf("Server started. PID=%d, generation=%llu, cores=%u, threads/pool=%zu, shutdown=%s. "
           "Waiting for requests...\n",
           getpid(), static_cast<unsigned long long>(server_generation),
           std::thread::hardware_concurrency(), threads_per_pool,
           (g_shutdown_mode == ShutdownMode::Drain) ? "drain" : "immediate");
    fflush(stdout);

    /* --- Dispatcher loop --- */
    while (g_running.load()) {
        sem_wait(g_server_sem);

        if (g_status_requested.exchange(false)) {
            time_t now = time(nullptr);
            long uptime = static_cast<long>(difftime(now, start_time));
            long hours = uptime / 3600;
            long mins  = (uptime % 3600) / 60;
            long secs  = uptime % 60;
            const char *mode_str = (g_shutdown_mode == ShutdownMode::Drain)
                                       ? "drain" : "immediate";

            int free_slots = 0, pending_slots = 0, proc_slots = 0, ready_slots = 0;
            sem_wait(g_mutex_sem);
            for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
                switch (g_shm->slots[i].state) {
                case IPC_SLOT_FREE:           ++free_slots;    break;
                case IPC_SLOT_REQUEST_PENDING: ++pending_slots; break;
                case IPC_SLOT_PROCESSING:      ++proc_slots;    break;
                case IPC_SLOT_RESPONSE_READY:  ++ready_slots;   break;
                }
            }
            sem_post(g_mutex_sem);

            printf("[STATUS] PID=%d, uptime=%ldh%02ldm%02lds, mode=%s, "
                   "threads/pool=%zu\n",
                   getpid(), hours, mins, secs, mode_str, threads_per_pool);
            printf("[STATUS] math_pool: %zu pending, string_pool: %zu pending\n",
                   math_pool.pending_count(), string_pool.pending_count());
            printf("[STATUS] slots: %d free, %d pending, %d processing, %d ready\n",
                   free_slots, pending_slots, proc_slots, ready_slots);
            fflush(stdout);
        }

        if (!g_running.load())
            break;

        sem_wait(g_mutex_sem);
        for (int i = 0; i < IPC_MAX_SLOTS; ++i) {
            if (g_shm->slots[i].state == IPC_SLOT_REQUEST_PENDING) {
                g_shm->slots[i].state = IPC_SLOT_PROCESSING;
                ipc_cmd_t cmd = g_shm->slots[i].command;

                sem_post(g_mutex_sem);

                switch (cmd) {
                case IPC_CMD_ADD:
                case IPC_CMD_SUB:
                case IPC_CMD_MUL:
                case IPC_CMD_DIV:
                    math_pool.submit(i);
                    break;
                case IPC_CMD_CONCAT:
                case IPC_CMD_SEARCH:
                    string_pool.submit(i);
                    break;
                }

                sem_wait(g_mutex_sem);
            }
        }
        sem_post(g_mutex_sem);
    }

    /* --- Shutdown --- */
    size_t pending = math_pool.pending_count() + string_pool.pending_count();

    if (g_shutdown_mode == ShutdownMode::Drain) {
        printf("\nShutdown requested (drain mode). "
               "%zu pending task(s) will be finished before exit.\n", pending);
    } else {
        printf("\nShutdown requested (immediate mode). "
               "Discarding pending task(s).\n");
    }
    fflush(stdout);

    size_t discarded_math   = math_pool.shutdown(g_shutdown_mode);
    size_t discarded_string = string_pool.shutdown(g_shutdown_mode);
    if (g_shutdown_mode == ShutdownMode::Immediate && (discarded_math + discarded_string) > 0) {
        printf("Discarded %zu task(s).\n", discarded_math + discarded_string);
    }

    cleanup_ipc();
    printf("Server shut down cleanly.\n");

    return 0;
}
