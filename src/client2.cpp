/**
 * @file client2.cpp
 * @brief Client 2: loads libipc.so via dlopen/dlsym. Operations: subtract, divide, search.
 */
#include "ipc_defs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

/* Function pointer typedefs for the library API */
typedef int  (*ipc_init_fn)(void);
typedef void (*ipc_cleanup_fn)(void);
typedef int  (*ipc_subtract_fn)(int32_t, int32_t, int32_t *);
typedef int  (*ipc_divide_fn)(int32_t, int32_t, uint64_t *);
typedef int  (*ipc_search_fn)(const char *, const char *, uint64_t *);
typedef int  (*ipc_get_result_fn)(uint64_t, ResponsePayload *, ipc_status_t *);

struct PendingRequest {
    uint64_t    id;
    ipc_cmd_t   cmd;
    std::string description;
    int32_t     a = 0;
    int32_t     b = 0;
    std::string s1;
    std::string s2;
};

static ipc_get_result_fn fn_get_result = nullptr;
static ipc_divide_fn fn_divide = nullptr;
static ipc_search_fn fn_search = nullptr;

static int resubmit_pending(PendingRequest &req)
{
    uint64_t new_id = 0;
    int rc = -1;
    if (req.cmd == IPC_CMD_DIV) {
        rc = fn_divide(req.a, req.b, &new_id);
    } else if (req.cmd == IPC_CMD_SEARCH) {
        rc = fn_search(req.s2.c_str(), req.s1.c_str(), &new_id);
    }
    if (rc == 0)
        req.id = new_id;
    return rc;
}

static void retry_pending_after_restart(std::vector<PendingRequest> &pending)
{
    if (pending.empty())
        return;

    for (auto &req : pending) {
        req.id = 0;
    }

    printf("\nNotice: server restart detected. Re-submitting %zu async request(s)...\n",
           pending.size());
    auto it = pending.begin();
    while (it != pending.end()) {
        int rc = resubmit_pending(*it);
        if (rc == 0) {
            printf("Re-submitted [%s], new request ID: %lu\n",
                   it->description.c_str(), static_cast<unsigned long>(it->id));
            ++it;
        } else if (rc == IPC_ERR_SERVER_RESTARTED) {
            printf("Server is still restarting; pending requests remain queued for retry.\n");
            ++it;
            break;
        } else {
            printf("Failed to re-submit [%s]; dropping this pending request.\n",
                   it->description.c_str());
            it = pending.erase(it);
        }
    }
}

static void check_pending(std::vector<PendingRequest> &pending)
{
    auto it = pending.begin();
    while (it != pending.end()) {
        if (it->id == 0) {
            int rc = resubmit_pending(*it);
            if (rc == 0) {
                printf("Re-submitted [%s], new request ID: %lu\n",
                       it->description.c_str(), static_cast<unsigned long>(it->id));
            } else if (rc != IPC_ERR_SERVER_RESTARTED) {
                printf("Failed to re-submit [%s]; dropping this pending request.\n",
                       it->description.c_str());
                it = pending.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        ResponsePayload result;
        ipc_status_t status;
        int rc = fn_get_result(it->id, &result, &status);
        if (rc == 0) {
            printf("\nReceiving response for request %lu [%s]\n",
                   static_cast<unsigned long>(it->id), it->description.c_str());
            if (it->cmd == IPC_CMD_DIV) {
                if (status == IPC_STATUS_OK)
                    printf("Result is %d!\n", result.math_result);
                else if (status == IPC_STATUS_DIV_BY_ZERO)
                    printf("Error: division by zero!\n");
                else
                    printf("Error: status=%d\n", status);
            } else if (it->cmd == IPC_CMD_SEARCH) {
                if (status == IPC_STATUS_OK)
                    printf("Result is: %d\n", result.position);
                else if (status == IPC_STATUS_NOT_FOUND)
                    printf("Substring not found.\n");
                else
                    printf("Error: status=%d\n", status);
            }
            it = pending.erase(it);
        } else if (rc == IPC_NOT_READY) {
            ++it;
        } else if (rc == IPC_ERR_SERVER_RESTARTED) {
            retry_pending_after_restart(pending);
            return;
        } else {
            printf("Error: request %lu not found.\n",
                   static_cast<unsigned long>(it->id));
            it = pending.erase(it);
        }
    }
}

static bool pre_menu_restart_probe(std::vector<PendingRequest> &pending)
{
    ResponsePayload probe_result{};
    ipc_status_t probe_status = IPC_STATUS_OK;
    int rc = fn_get_result(0, &probe_result, &probe_status);
    if (rc == IPC_ERR_SERVER_RESTARTED) {
        if (pending.empty()) {
            printf("\nNotice: server restart detected. Reconnected to fresh IPC state.\n");
        } else {
            retry_pending_after_restart(pending);
        }
        return true;
    }
    return false;
}

int main()
{
    /* Load library at runtime */
    void *handle = dlopen("./libipc.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto fn_init      = reinterpret_cast<ipc_init_fn>(dlsym(handle, "ipc_init"));
    auto fn_cleanup   = reinterpret_cast<ipc_cleanup_fn>(dlsym(handle, "ipc_cleanup"));
    auto fn_subtract  = reinterpret_cast<ipc_subtract_fn>(dlsym(handle, "ipc_subtract"));
    fn_divide         = reinterpret_cast<ipc_divide_fn>(dlsym(handle, "ipc_divide"));
    fn_search         = reinterpret_cast<ipc_search_fn>(dlsym(handle, "ipc_search"));
    fn_get_result     = reinterpret_cast<ipc_get_result_fn>(dlsym(handle, "ipc_get_result"));

    if (!fn_init || !fn_cleanup || !fn_subtract || !fn_divide ||
        !fn_search || !fn_get_result) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    if (fn_init() != 0) {
        fprintf(stderr, "Failed to connect to server. Is it running?\n");
        dlclose(handle);
        return 1;
    }

    std::vector<PendingRequest> pending;
    bool running = true;

    while (running) {
        check_pending(pending);
        if (pre_menu_restart_probe(pending))
            continue;

        printf("\n1. Subtract 2 numbers        (blocking)\n"
               "2. Divide 2 numbers          (non-blocking)\n"
               "3. Find substring in string  (non-blocking)\n"
               "4. Check pending results\n"
               "5. Exit\n\n"
               "Enter command: ");
        fflush(stdout);

        int choice = 0;
        if (scanf("%d", &choice) != 1) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            printf("Invalid input.\n");
            continue;
        }
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {}

        switch (choice) {
        case 1: {
            int32_t a, b;
            printf("Enter operand 1: ");
            fflush(stdout);
            if (scanf("%d", &a) != 1) { printf("Invalid input.\n"); break; }
            printf("Enter operand 2: ");
            fflush(stdout);
            if (scanf("%d", &b) != 1) { printf("Invalid input.\n"); break; }
            while ((c = getchar()) != '\n' && c != EOF) {}

            printf("\nSending request...\n");
            int32_t result;
            int rc = fn_subtract(a, b, &result);
            if (rc == 0) {
                printf("Receiving response...\n");
                printf("Result is %d!\n", result);
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted; blocking request was not retried. "
                       "Please run the command again.\n");
            } else {
                printf("Error: subtract operation failed.\n");
            }
            break;
        }
        case 2: {
            int32_t a, b;
            printf("Enter operand 1: ");
            fflush(stdout);
            if (scanf("%d", &a) != 1) { printf("Invalid input.\n"); break; }
            printf("Enter operand 2: ");
            fflush(stdout);
            if (scanf("%d", &b) != 1) { printf("Invalid input.\n"); break; }
            while ((c = getchar()) != '\n' && c != EOF) {}

            uint64_t req_id;
            char desc[64];
            snprintf(desc, sizeof(desc), "%d/%d", a, b);
            printf("\nSending request (%s) ... ", desc);
            fflush(stdout);
            int rc = fn_divide(a, b, &req_id);
            if (rc == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_DIV, desc, a, b, "", ""});
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted while submitting; reconnected. "
                       "Please retry this command.\n");
            } else {
                printf("Error: divide operation failed.\n");
            }
            break;
        }
        case 3: {
            char s1[IPC_MAX_STRING_LEN + 2], s2[IPC_MAX_STRING_LEN + 2];
            printf("Enter substring: ");
            fflush(stdout);
            if (!fgets(s1, sizeof(s1), stdin)) { printf("Invalid input.\n"); break; }
            s1[strcspn(s1, "\n")] = '\0';
            printf("Enter string: ");
            fflush(stdout);
            if (!fgets(s2, sizeof(s2), stdin)) { printf("Invalid input.\n"); break; }
            s2[strcspn(s2, "\n")] = '\0';

            uint64_t req_id;
            char desc[128];
            snprintf(desc, sizeof(desc), "search('%s' in '%s')", s1, s2);
            printf("\nSending request %s ... ", desc);
            fflush(stdout);
            /* Note: search(haystack, needle) -- s2 is the string, s1 is the substring */
            int rc = fn_search(s2, s1, &req_id);
            if (rc == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_SEARCH, desc, 0, 0, s1, s2});
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted while submitting; reconnected. "
                       "Please retry this command.\n");
            } else {
                printf("Error: search failed (strings must be 1..16 chars).\n");
            }
            break;
        }
        case 4:
            if (pending.empty()) {
                printf("No pending requests.\n");
            } else {
                printf("Checking %zu pending request(s)...\n", pending.size());
                check_pending(pending);
                if (!pending.empty())
                    printf("%zu request(s) still processing.\n", pending.size());
            }
            break;
        case 5:
            running = false;
            break;
        default:
            printf("Unknown command.\n");
            break;
        }
    }

    fn_cleanup();
    dlclose(handle);
    printf("Client 2 exiting.\n");
    return 0;
}
