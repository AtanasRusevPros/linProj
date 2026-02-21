/**
 * @file server.cpp
 * @brief IPC server: creates shared memory, dispatches requests to thread pools.
 */
#include "ipc_defs.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <queue>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

    void submit(int slot_index)
    {
        {
            std::scoped_lock lock(mutex_);
            queue_.push(slot_index);
        }
        cv_.notify_one();
    }

    void shutdown()
    {
        if (stop_.exchange(true))
            return;
        cv_.notify_all();
        for (auto &w : workers_) {
            if (w.joinable())
                w.join();
        }
    }

private:
    std::vector<std::thread>     workers_;
    std::queue<int>              queue_;
    std::mutex                   mutex_;
    std::condition_variable      cv_;
    std::atomic<bool>            stop_{false};
    std::function<void(int)>     task_handler_;
};

/* ================================================================== */
/*  Global state                                                       */
/* ================================================================== */

static std::atomic<bool> g_running{true};
static SharedMemoryLayout *g_shm = nullptr;
static int g_shm_fd = -1;
static sem_t *g_mutex_sem = nullptr;
static sem_t *g_server_sem = nullptr;
static sem_t *g_slot_sems[IPC_MAX_SLOTS] = {};

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
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

int main()
{
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

    memset(g_shm, 0, sizeof(SharedMemoryLayout));
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

    /* --- Thread pools --- */
    ThreadPool math_pool(2, process_math);
    ThreadPool string_pool(2, process_string);

    printf("Server started. PID=%d. Waiting for requests...\n", getpid());
    fflush(stdout);

    /* --- Dispatcher loop --- */
    while (g_running.load()) {
        sem_wait(g_server_sem);
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
    printf("\nShutting down server...\n");
    math_pool.shutdown();
    string_pool.shutdown();
    cleanup_ipc();
    printf("Server shut down cleanly.\n");

    return 0;
}
