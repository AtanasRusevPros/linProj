#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include "ipc_defs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct PendingRequest {
    uint64_t    id;
    ipc_cmd_t   cmd;
    std::string description;
    int32_t     a = 0;
    int32_t     b = 0;
    std::string s1;
    std::string s2;
};

static inline void clear_input_line(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
}

static inline bool read_menu_choice(int *choice)
{
    if (scanf("%d", choice) != 1) {
        clear_input_line();
        printf("Invalid input.\n");
        return false;
    }
    clear_input_line();
    return true;
}

static inline bool read_two_ints(int32_t *a, int32_t *b)
{
    printf("Enter operand 1: ");
    fflush(stdout);
    if (scanf("%d", a) != 1) {
        printf("Invalid input.\n");
        clear_input_line();
        return false;
    }

    printf("Enter operand 2: ");
    fflush(stdout);
    if (scanf("%d", b) != 1) {
        printf("Invalid input.\n");
        clear_input_line();
        return false;
    }

    clear_input_line();
    return true;
}

static inline bool read_short_string(const char *prompt, char *out, size_t out_size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, static_cast<int>(out_size), stdin)) {
        printf("Invalid input.\n");
        return false;
    }

    size_t len = strcspn(out, "\n");
    if (out[len] == '\n') {
        out[len] = '\0';
    } else {
        clear_input_line();
    }
    return true;
}

using ResubmitPendingFn = int (*)(PendingRequest &);
using GetResultFn = int (*)(uint64_t, ResponsePayload *, ipc_status_t *);

static inline void retry_pending_after_restart(std::vector<PendingRequest> &pending,
                                               ResubmitPendingFn resubmit_pending)
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
            break;
        } else {
            printf("Failed to re-submit [%s]; dropping this pending request.\n",
                   it->description.c_str());
            it = pending.erase(it);
        }
    }
}

static inline bool pre_menu_restart_probe(std::vector<PendingRequest> &pending,
                                          GetResultFn get_result,
                                          ResubmitPendingFn resubmit_pending)
{
    ResponsePayload probe_result{};
    ipc_status_t probe_status = IPC_STATUS_OK;
    int rc = get_result(0, &probe_result, &probe_status);
    if (rc == IPC_ERR_SERVER_RESTARTED) {
        if (pending.empty()) {
            printf("\nNotice: server restart detected. Reconnected to fresh IPC state.\n");
        } else {
            retry_pending_after_restart(pending, resubmit_pending);
        }
        return true;
    }
    return false;
}

#endif /* CLIENT_COMMON_H */
