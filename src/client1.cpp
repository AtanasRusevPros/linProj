/**
 * @file client1.cpp
 * @brief Client 1: links libipc.so directly. Operations: add, multiply, concat.
 */
#include "libipc.h"
#include "client_common.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int resubmit_pending(PendingRequest &req)
{
    uint64_t new_id = 0;
    int rc = -1;
    if (req.cmd == IPC_CMD_MUL) {
        rc = ipc_multiply(req.a, req.b, &new_id);
    } else if (req.cmd == IPC_CMD_CONCAT) {
        rc = ipc_concat(req.s1.c_str(), req.s2.c_str(), &new_id);
    }
    if (rc == 0)
        req.id = new_id;
    return rc;
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
        int rc = ipc_get_result(it->id, &result, &status);
        if (rc == 0) {
            printf("\nReceiving response for request %lu [%s]\n",
                   static_cast<unsigned long>(it->id), it->description.c_str());
            if (status == IPC_STATUS_OK) {
                if (it->cmd == IPC_CMD_MUL)
                    printf("Result is %d!\n", result.math_result);
                else if (it->cmd == IPC_CMD_CONCAT)
                    printf("Result is %s!\n", result.str_result);
            } else if (status == IPC_STATUS_STR_TOO_LONG) {
                printf("Error: string too long.\n");
            } else {
                printf("Error: status=%d\n", status);
            }
            it = pending.erase(it);
        } else if (rc == IPC_NOT_READY) {
            ++it;
        } else if (rc == IPC_ERR_SERVER_RESTARTED) {
            retry_pending_after_restart(pending, resubmit_pending);
            return;
        } else {
            printf("Error: request %lu not found.\n",
                   static_cast<unsigned long>(it->id));
            it = pending.erase(it);
        }
    }
}

int main()
{
    if (ipc_init() != 0) {
        fprintf(stderr, "Failed to connect to server. Is it running?\n");
        return 1;
    }

    std::vector<PendingRequest> pending;
    bool running = true;

    while (running) {
        if (pre_menu_restart_probe(pending, ipc_get_result, resubmit_pending))
            continue;

        printf("\n1. Add 2 numbers          (blocking)\n"
               "2. Multiply 2 numbers     (non-blocking)\n"
               "3. Concatenate 2 strings  (non-blocking)\n"
               "4. Check pending results\n"
               "5. Exit\n\n"
               "Enter command: ");
        fflush(stdout);

        int choice = 0;
        if (!read_menu_choice(&choice))
            continue;

        switch (choice) {
        case 1: {
            int32_t a, b;
            if (!read_two_ints(&a, &b))
                break;

            printf("\nSending request...\n");
            int32_t result;
            int rc = ipc_add(a, b, &result);
            if (rc == 0) {
                printf("Receiving response...\n");
                printf("Result is %d!\n", result);
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted; blocking request was not retried. "
                       "Please run the command again.\n");
            } else {
                printf("Error: add operation failed.\n");
            }
            break;
        }
        case 2: {
            int32_t a, b;
            if (!read_two_ints(&a, &b))
                break;

            uint64_t req_id;
            char desc[64];
            snprintf(desc, sizeof(desc), "%d*%d", a, b);
            printf("\nSending request (%s) ... ", desc);
            fflush(stdout);
            int rc = ipc_multiply(a, b, &req_id);
            if (rc == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_MUL, desc, a, b, "", ""});
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted while submitting; reconnected. "
                       "Please retry this command.\n");
            } else {
                printf("Error: multiply operation failed.\n");
            }
            break;
        }
        case 3: {
            char s1[IPC_MAX_STRING_LEN + 2], s2[IPC_MAX_STRING_LEN + 2];
            if (!read_short_string("Enter string 1: ", s1, sizeof(s1)))
                break;
            if (!read_short_string("Enter string 2: ", s2, sizeof(s2)))
                break;

            uint64_t req_id;
            char desc[128];
            snprintf(desc, sizeof(desc), "concat(%s,%s)", s1, s2);
            printf("\nSending request %s ... ", desc);
            fflush(stdout);
            int rc = ipc_concat(s1, s2, &req_id);
            if (rc == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_CONCAT, desc, 0, 0, s1, s2});
            } else if (rc == IPC_ERR_SERVER_RESTARTED) {
                printf("Server restarted while submitting; reconnected. "
                       "Please retry this command.\n");
            } else {
                printf("Error: concat failed (strings must be 1..16 chars).\n");
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

    ipc_cleanup();
    printf("Client 1 exiting.\n");
    return 0;
}
