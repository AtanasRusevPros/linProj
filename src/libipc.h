/**
 * @file libipc.h
 * @brief Public C API for the IPC communication library (libipc.so).
 *
 * This header exposes blocking and non-blocking IPC calls for clients.
 * All functions use C linkage (extern "C") so the library can be loaded
 * via dlopen/dlsym as well as linked directly.
 */
#ifndef LIBIPC_H
#define LIBIPC_H

#include "ipc_defs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize client-side connection to shared memory and semaphores.
 *
 * Must be called before any other ipc_ function. The server must already
 * be running (it creates the shared memory segment).
 * If the server restarts while a client is running, calls may return
 * IPC_ERR_SERVER_RESTARTED and the caller should retry as appropriate.
 *
 * @return 0 on success, -1 on failure.
 */
int ipc_init(void);

/**
 * @brief Disconnect and release local mappings.
 *
 * Calls munmap and sem_close. Does NOT unlink IPC objects (the server
 * owns that responsibility).
 */
void ipc_cleanup(void);

/* ------------------------------------------------------------------ */
/*  Blocking (synchronous) calls -- add, subtract                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Add two 32-bit signed integers (blocking).
 *
 * Sends the request and blocks until the server returns the result.
 *
 * @param[in]  a       First operand.
 * @param[in]  b       Second operand.
 * @param[out] result  Pointer to store the sum.
 * @return 0 on success, -1 on error, IPC_ERR_SERVER_RESTARTED if the server
 *         restarted and this request context was invalidated.
 */
int ipc_add(int32_t a, int32_t b, int32_t *result);

/**
 * @brief Subtract two 32-bit signed integers (blocking).
 *
 * @param[in]  a       First operand.
 * @param[in]  b       Second operand.
 * @param[out] result  Pointer to store (a - b).
 * @return 0 on success, -1 on error, IPC_ERR_SERVER_RESTARTED if the server
 *         restarted and this request context was invalidated.
 */
int ipc_subtract(int32_t a, int32_t b, int32_t *result);

/* ------------------------------------------------------------------ */
/*  Non-blocking (asynchronous) calls                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Multiply two 32-bit signed integers (non-blocking).
 *
 * Returns immediately with a request ID. Use ipc_get_result() to
 * retrieve the result later.
 *
 * @param[in]  a           First operand.
 * @param[in]  b           Second operand.
 * @param[out] request_id  Pointer to store the assigned request ID.
 * @return 0 on success, -1 on error, IPC_ERR_SERVER_RESTARTED if the server
 *         restarted and this request context was invalidated.
 */
int ipc_multiply(int32_t a, int32_t b, uint64_t *request_id);

/**
 * @brief Divide two 32-bit signed integers (non-blocking).
 *
 * Division by zero is detected by the server and reported as
 * IPC_STATUS_DIV_BY_ZERO in the response status.
 *
 * @param[in]  a           Dividend.
 * @param[in]  b           Divisor.
 * @param[out] request_id  Pointer to store the assigned request ID.
 * @return 0 on success, -1 on error, IPC_ERR_SERVER_RESTARTED if the server
 *         restarted and this request context was invalidated.
 */
int ipc_divide(int32_t a, int32_t b, uint64_t *request_id);

/**
 * @brief Concatenate two strings (non-blocking).
 *
 * Each string must be 1..16 characters. The result can be up to 32 chars.
 *
 * @param[in]  s1          First string.
 * @param[in]  s2          Second string.
 * @param[out] request_id  Pointer to store the assigned request ID.
 * @return 0 on success, -1 on error (e.g., string too long or empty),
 *         IPC_ERR_SERVER_RESTARTED if the server restarted.
 */
int ipc_concat(const char *s1, const char *s2, uint64_t *request_id);

/**
 * @brief Search for a substring within a string (non-blocking).
 *
 * Finds the 0-indexed starting position of needle in haystack.
 * If not found, the server sets status to IPC_STATUS_NOT_FOUND and
 * the position field to -1.
 *
 * @param[in]  haystack    The string to search in (1..16 chars).
 * @param[in]  needle      The substring to find (1..16 chars).
 * @param[out] request_id  Pointer to store the assigned request ID.
 * @return 0 on success, -1 on error, IPC_ERR_SERVER_RESTARTED if the server
 *         restarted and this request context was invalidated.
 */
int ipc_search(const char *haystack, const char *needle, uint64_t *request_id);

/**
 * @brief Poll for the result of a non-blocking call.
 *
 * @param[in]  request_id  The request ID returned by the async call.
 * @param[out] result      Pointer to store the response payload.
 * @param[out] status      Pointer to store the response status code.
 * @return 0 if result is ready, IPC_NOT_READY if still processing,
 *         IPC_ERR_SERVER_RESTARTED if the server restarted and old request IDs
 *         were invalidated, -1 on other errors.
 */
int ipc_get_result(uint64_t request_id, ResponsePayload *result,
                   ipc_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* LIBIPC_H */
