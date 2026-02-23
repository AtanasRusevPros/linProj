/**
 * @file ipc_defs.h
 * @brief Shared IPC protocol definitions: message structures, enums, constants.
 *
 * This header defines the communication protocol between clients and the server.
 * All types are POD (Plain Old Data) and safe for use in POSIX shared memory.
 * This is a pure C header -- no C++ types.
 */
#ifndef IPC_DEFS_H
#define IPC_DEFS_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of concurrent in-flight requests (slot count). */
#define IPC_MAX_SLOTS       16

/** Maximum length of an input string (excluding null terminator). */
#define IPC_MAX_STRING_LEN  16

/** Maximum length of a result string (two concatenated strings + null). */
#define IPC_MAX_RESULT_LEN  33

/** Return code from ipc_get_result() when the result is not yet available. */
#define IPC_NOT_READY       1

/** Return code when server restart is detected and request context was invalidated. */
#define IPC_ERR_SERVER_RESTARTED -2

/* --- IPC object names (POSIX shared memory and semaphores) --- */

#define IPC_SHM_NAME        "/ipc_shm"
#define IPC_MUTEX_NAME      "/ipc_mutex"
#define IPC_SERVER_SEM_NAME "/ipc_server_notify"
#define IPC_SLOT_SEM_PREFIX "/ipc_slot_"

/**
 * @brief Command types for IPC operations.
 */
typedef enum {
    IPC_CMD_ADD = 0,
    IPC_CMD_SUB,
    IPC_CMD_MUL,
    IPC_CMD_DIV,
    IPC_CMD_CONCAT,
    IPC_CMD_SEARCH
} ipc_cmd_t;

/**
 * @brief Status codes returned in IPC responses.
 */
typedef enum {
    IPC_STATUS_OK = 0,
    IPC_STATUS_DIV_BY_ZERO,
    IPC_STATUS_NOT_FOUND,
    IPC_STATUS_STR_TOO_LONG,
    IPC_STATUS_INVALID_INPUT,
    IPC_STATUS_INTERNAL_ERROR
} ipc_status_t;

/**
 * @brief State of a message slot in shared memory.
 */
typedef enum {
    IPC_SLOT_FREE = 0,
    IPC_SLOT_REQUEST_PENDING,
    IPC_SLOT_PROCESSING,
    IPC_SLOT_RESPONSE_READY
} ipc_slot_state_t;

/**
 * @brief Arguments for math operations (32-bit signed integers).
 */
typedef struct {
    int32_t a;
    int32_t b;
} MathArgs;

/**
 * @brief Arguments for string operations.
 *
 * Each string can be 1..16 characters long (plus null terminator).
 */
typedef struct {
    char s1[IPC_MAX_STRING_LEN + 1];
    char s2[IPC_MAX_STRING_LEN + 1];
} StringArgs;

/**
 * @brief Request payload -- a union of math or string arguments.
 */
typedef union {
    MathArgs   math;
    StringArgs str;
} RequestPayload;

/**
 * @brief Response payload -- a union of possible result types.
 */
typedef union {
    int32_t math_result;
    char    str_result[IPC_MAX_RESULT_LEN];
    int32_t position;
} ResponsePayload;

/**
 * @brief A single message slot in shared memory.
 *
 * Each slot holds one in-flight request and its corresponding response.
 * The slot transitions through states:
 *   FREE -> REQUEST_PENDING -> PROCESSING -> RESPONSE_READY -> FREE
 */
typedef struct {
    ipc_slot_state_t state;
    uint64_t         request_id;
    pid_t            client_pid;
    ipc_cmd_t        command;
    RequestPayload   request;
    ResponsePayload  response;
    ipc_status_t     status;
} MessageSlot;

/**
 * @brief Layout of the entire shared memory region.
 */
typedef struct {
    uint64_t    server_generation;
    uint64_t    next_request_id;
    MessageSlot slots[IPC_MAX_SLOTS];
} SharedMemoryLayout;

#ifdef __cplusplus
}
#endif

#endif /* IPC_DEFS_H */
