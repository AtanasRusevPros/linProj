/**
 * @file client1.cpp
 * @brief Client 1: links libipc.so directly. Operations: add, multiply, concat.
 */
#include "libipc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct PendingRequest {
    uint64_t    id;
    ipc_cmd_t   cmd;
    std::string description;
};

static void check_pending(std::vector<PendingRequest> &pending)
{
    auto it = pending.begin();
    while (it != pending.end()) {
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
        check_pending(pending);

        printf("\n1. Add 2 numbers          (blocking)\n"
               "2. Multiply 2 numbers     (non-blocking)\n"
               "3. Concatenate 2 strings  (non-blocking)\n"
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
            if (ipc_add(a, b, &result) == 0) {
                printf("Receiving response...\n");
                printf("Result is %d!\n", result);
            } else {
                printf("Error: add operation failed.\n");
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
            snprintf(desc, sizeof(desc), "%d*%d", a, b);
            printf("\nSending request (%s) ... ", desc);
            fflush(stdout);
            if (ipc_multiply(a, b, &req_id) == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_MUL, desc});
            } else {
                printf("Error: multiply operation failed.\n");
            }
            break;
        }
        case 3: {
            char s1[IPC_MAX_STRING_LEN + 2], s2[IPC_MAX_STRING_LEN + 2];
            printf("Enter string 1: ");
            fflush(stdout);
            if (!fgets(s1, sizeof(s1), stdin)) { printf("Invalid input.\n"); break; }
            s1[strcspn(s1, "\n")] = '\0';
            printf("Enter string 2: ");
            fflush(stdout);
            if (!fgets(s2, sizeof(s2), stdin)) { printf("Invalid input.\n"); break; }
            s2[strcspn(s2, "\n")] = '\0';

            uint64_t req_id;
            char desc[128];
            snprintf(desc, sizeof(desc), "concat(%s,%s)", s1, s2);
            printf("\nSending request %s ... ", desc);
            fflush(stdout);
            if (ipc_concat(s1, s2, &req_id) == 0) {
                printf("Request ID: %lu\n", static_cast<unsigned long>(req_id));
                pending.push_back({req_id, IPC_CMD_CONCAT, desc});
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
