//
// Created by Mariano Aponte on 30/11/23.
//

#ifndef RDMACM_KTMRDMA_H
#define RDMACM_KTMRDMA_H

#include <errno.h>
#include <string>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <cstdint>          // for uintptr_t
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

#define DEBUG

void Testlib();

/**
 * @brief Wrapper function for rdma_getaddrinfo with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_getaddrinfo(char *node, char *service, struct rdma_addrinfo *hints, struct rdma_addrinfo **res);

/**
 * @brief Wrapper function for rdma_create_ep with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_create_ep(struct rdma_cm_id **id, struct rdma_addrinfo *res, struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr);

/**
 * @brief Wrapper function for rdma_reg_msgs with error handling.
 *
 * @return A pointer to the registered memory region.
 * @throws std::runtime_error on failure
 */
ibv_mr * ktm_rdma_reg_msgs(struct rdma_cm_id *id, void *addr, size_t length);

/**
 * @brief Wrapper function for rdma_reg_read with error handling.
 *
 * @return A pointer to the memory region associated with the registered buffer.
 * @throws std::runtime_error on failure
 */
ibv_mr * ktm_rdma_reg_read(struct rdma_cm_id *id, void *addr, size_t length);

/**
 * @brief Wrapper function for rdma_reg_write with error handling.
 *
 * @return A pointer to the memory region associated with the registered buffer.
 * @throws std::runtime_error on failure
 */
ibv_mr * ktm_rdma_reg_write(struct rdma_cm_id *id, void *addr, size_t length);

/**
 * @brief Wrapper function for rdma_connect with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);

/**
 * @brief Wrapper function for rdma_listen with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_listen(struct rdma_cm_id *id, int backlog);

/**
 * @brief Wrapper function for rdma_get_request with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_get_request(struct rdma_cm_id *listen, struct rdma_cm_id **id);

/**
 * @brief Wrapper function for rdma_accept with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);

/**
 * @brief Wrapper function for rdma_post_recv with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_post_recv(struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr);

/**
 * @brief Wrapper function for rdma_post_send with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_post_send (struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags);

/**
 * @brief Wrapper for rdma_get_send_comp with error handling.
 *
 * @return The number of completions returned by rdma_get_send_comp.
 * @throws std::runtime_error on failure or completion error status
 */
int ktm_rdma_get_send_comp(struct rdma_cm_id *id, struct ibv_wc *wc);

/**
 * @brief Wrapper for rdma_get_recv_comp with error handling.
 *
 * @return The number of completions returned by rdma_get_recv_comp.
 * @throws std::runtime_error on failure or completion error status
 */
int ktm_rdma_get_recv_comp(struct rdma_cm_id *id, struct ibv_wc *wc);

/**
 * @brief Wrapper function for rdma_post_read with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_post_read(struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey);

/**
 * @brief Wrapper function for rdma_post_write with error handling.
 *
 * @throws std::runtime_error on failure
 */
void ktm_rdma_post_write (struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey);

// Address/RKey exchange helpers over the MSG channel
void ktm_send_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey);
void ktm_recv_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey);

void ktm_client_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr,
                                   uintptr_t * remote_addr, uint32_t * remote_rkey);
void ktm_client_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr,
                                   uintptr_t * remote_addr, uint32_t * remote_rkey, struct ibv_mr *rdma_mr);

void ktm_server_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr,
                                   uintptr_t * remote_addr, uint32_t * remote_rkey);
void ktm_server_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr,
                                   uintptr_t * remote_addr, uint32_t * remote_rkey, struct ibv_mr *rdma_mr);

// ---- Inline helpers (no per-post ibv_query_qp) --------------------------------

/**
 * @brief Query max_inline_data once for the QP behind @p id.
 *
 * Looks at both init_attr.cap.max_inline_data and attr.cap.max_inline_data and returns
 * whichever is non-zero (preferring init_attr).
 *
 * @return The negotiated inline capability in bytes (0 if unsupported).
 * @throws std::runtime_error if ibv_query_qp fails.
 */
uint32_t ktm_rdma_query_max_inline(struct rdma_cm_id *id);

/**
 * @brief Post an INLINE SEND using a cached max_inline value.
 *
 * This function does NOT call ibv_query_qp; the caller must pass a valid @p max_inline
 * cached at connection/accept time. Throws if @p length exceeds @p max_inline.
 *
 * @param id        rdma_cm_id whose QP to post on.
 * @param context   User context (returned in CQE wr_id).
 * @param addr      Pointer to payload to inline.
 * @param length    Payload size in bytes.
 * @param flags     Additional send flags (e.g., IBV_SEND_SIGNALED).
 * @param max_inline Cached inline limit for this QP (bytes).
 *
 * @throws std::runtime_error on error or if length > max_inline.
 */
void ktm_rdma_post_send_inline_cached(struct rdma_cm_id *id,
                                      void *context,
                                      const void *addr,
                                      size_t length,
                                      int flags,
                                      uint32_t max_inline);

#endif //RDMACM_KTMRDMA_H
