#include "ktmrdma.h"
#include <cerrno>
#include <cstdio>
#include <cstring>   // std::memcpy, std::strerror
#include <netdb.h>   // gai_strerror

// ---- Resolver ----------------------------------------------------------------

void ktm_rdma_getaddrinfo(char *node, char *service, struct rdma_addrinfo *hints, struct rdma_addrinfo **res) {
#ifdef DEBUG
    std::cout << "rdma_getaddrinfo(" << (node ? node : "(null)") << ", " << (service ? service : "(null)") << ")\n";
#endif
    int rc = rdma_getaddrinfo(node, service, hints, res);
    if (rc == -1) {
        throw std::runtime_error(std::string("rdma_getaddrinfo(): errno=") + std::strerror(errno));
    } else if (rc != 0) {
        throw std::runtime_error(std::string("rdma_getaddrinfo(): ") + gai_strerror(rc));
    }
}

// ---- Endpoint / QP -----------------------------------------------------------

void ktm_rdma_create_ep(struct rdma_cm_id **id, struct rdma_addrinfo *res, struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) {
    if (rdma_create_ep(id, res, pd, qp_init_attr) == -1) {
        throw std::runtime_error(std::string("rdma_create_ep(): ") + std::strerror(errno));
    }
}

void ktm_rdma_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param) {
    if (rdma_connect(id, conn_param) == -1) {
        throw std::runtime_error(std::string("rdma_connect(): ") + std::strerror(errno));
    }
}

void ktm_rdma_listen(struct rdma_cm_id *id, int backlog) {
    if (rdma_listen(id, backlog) == -1) {
        throw std::runtime_error(std::string("rdma_listen(): ") + std::strerror(errno));
    }
}

void ktm_rdma_get_request(struct rdma_cm_id *listen, struct rdma_cm_id **id) {
    if (rdma_get_request(listen, id) == -1) {
        throw std::runtime_error(std::string("rdma_get_request(): ") + std::strerror(errno));
    }
}

void ktm_rdma_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param) {
    if (rdma_accept(id, conn_param) == -1) {
        throw std::runtime_error(std::string("rdma_accept(): ") + std::strerror(errno));
    }
}

// ---- Memory ------------------------------------------------------------------

ibv_mr * ktm_rdma_reg_msgs(struct rdma_cm_id *id, void *addr, size_t length) {
    ibv_mr *mr = rdma_reg_msgs(id, addr, length);
    if (!mr) throw std::runtime_error(std::string("rdma_reg_msgs(): ") + std::strerror(errno));
    return mr;
}

ibv_mr * ktm_rdma_reg_read(struct rdma_cm_id *id, void *addr, size_t length) {
    ibv_mr *mr = rdma_reg_read(id, addr, length);
    if (!mr) throw std::runtime_error(std::string("rdma_reg_read(): ") + std::strerror(errno));
    return mr;
}

ibv_mr * ktm_rdma_reg_write(struct rdma_cm_id *id, void *addr, size_t length) {
    ibv_mr *mr = rdma_reg_write(id, addr, length);
    if (!mr) throw std::runtime_error(std::string("rdma_reg_write(): ") + std::strerror(errno));
    return mr;
}

// ---- Posting / completions ----------------------------------------------------

void ktm_rdma_post_recv (struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr) {
    if (rdma_post_recv(id, context, addr, length, mr) == -1) {
        throw std::runtime_error(std::string("rdma_post_recv(): ") + std::strerror(errno));
    }
}

void ktm_rdma_post_send (struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags) {
    if (rdma_post_send(id, context, addr, length, mr, flags) == -1) {
        throw std::runtime_error(std::string("rdma_post_send(): ") + std::strerror(errno));
    }
}

int ktm_rdma_get_send_comp(struct rdma_cm_id *id, struct ibv_wc *wc) {
    int n = rdma_get_send_comp(id, wc);
    if (n < 0) throw std::runtime_error(std::string("rdma_get_send_comp(): ") + std::strerror(errno));
    if (wc->status != IBV_WC_SUCCESS) {
        throw std::runtime_error(std::string("rdma_get_send_comp(): completion error: ") + ibv_wc_status_str(wc->status));
    }
    return n;
}

int ktm_rdma_get_recv_comp(struct rdma_cm_id *id, struct ibv_wc *wc) {
    int n = rdma_get_recv_comp(id, wc);
    if (n < 0) throw std::runtime_error(std::string("rdma_get_recv_comp(): ") + std::strerror(errno));
    if (wc->status != IBV_WC_SUCCESS) {
        throw std::runtime_error(std::string("rdma_get_recv_comp(): completion error: ") + ibv_wc_status_str(wc->status));
    }
    return n;
}

void ktm_rdma_post_read(struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey) {
    if (rdma_post_read(id, context, addr, length, mr, flags, remote_addr, rkey) == -1) {
        throw std::runtime_error(std::string("rdma_post_read(): ") + std::strerror(errno));
    }
}

void ktm_rdma_post_write(struct rdma_cm_id *id, void *context, void *addr, size_t length, struct ibv_mr *mr, int flags, uint64_t remote_addr, uint32_t rkey) {
    if (rdma_post_write(id, context, addr, length, mr, flags, remote_addr, rkey) == -1) {
        throw std::runtime_error(std::string("rdma_post_write(): ") + std::strerror(errno));
    }
}

// ---- Address/RKey exchange over MSG ------------------------------------------

void ktm_rdma_send_address(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr) {
    auto local_int_address = (uintptr_t) mr->addr;
    std::memcpy(addr, &local_int_address, sizeof(local_int_address));
    ktm_rdma_post_send(id, nullptr, addr, length, mr, 0);
    ibv_wc wc{}; (void)ktm_rdma_get_send_comp(id, &wc);
}

void ktm_rdma_send_address(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, struct ibv_mr *rdma_mr) {
    auto local_int_address = (uintptr_t) rdma_mr->addr;
    std::memcpy(addr, &local_int_address, sizeof(local_int_address));
    ktm_rdma_post_send(id, nullptr, addr, length, mr, 0);
    ibv_wc wc{}; (void)ktm_rdma_get_send_comp(id, &wc);
}

uintptr_t ktm_rdma_get_address(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr) {
    ktm_rdma_post_recv(id, nullptr, addr, length, mr);
    ibv_wc wc{}; (void)ktm_rdma_get_recv_comp(id, &wc);
    uintptr_t peer_int_address = 0;
    std::memcpy(&peer_int_address, addr, sizeof(peer_int_address));
    return peer_int_address;
}

void ktm_rdma_send_rkey(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr) {
    auto local_rkey = mr->rkey;
    std::memcpy(addr, &local_rkey, sizeof(local_rkey));
    ktm_rdma_post_send(id, nullptr, addr, length, mr, 0);
    ibv_wc wc{}; (void)ktm_rdma_get_send_comp(id, &wc);
}

void ktm_rdma_send_rkey(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, struct ibv_mr *rdma_mr) {
    auto local_rkey = rdma_mr->rkey;
    std::memcpy(addr, &local_rkey, sizeof(local_rkey));
    ktm_rdma_post_send(id, nullptr, addr, length, mr, 0);
    ibv_wc wc{}; (void)ktm_rdma_get_send_comp(id, &wc);
}

uint32_t ktm_rdma_get_rkey(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr) {
    ktm_rdma_post_recv(id, nullptr, addr, length, mr);
    ibv_wc wc{}; (void)ktm_rdma_get_recv_comp(id, &wc);
    uint32_t peer_rkey = 0;
    std::memcpy(&peer_rkey, addr, sizeof(peer_rkey));
    return peer_rkey;
}

void ktm_send_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey) {
    ktm_rdma_send_address(id, addr, length, mr);
    ktm_rdma_send_rkey(id, addr, length, mr);
}

void ktm_recv_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey) {
    uintptr_t received_addr = ktm_rdma_get_address(id, addr, length, mr);
    std::memcpy(remote_addr, &received_addr, sizeof(uintptr_t));
    uint32_t received_rkey = ktm_rdma_get_rkey(id, addr, length, mr);
    std::memcpy(remote_rkey, &received_rkey, sizeof(uint32_t));
}

void ktm_client_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey) {
    ktm_rdma_send_address(id, addr, length, mr);
    uintptr_t received_addr = ktm_rdma_get_address(id, addr, length, mr);
    std::memcpy(remote_addr, &received_addr, sizeof(uintptr_t));
    ktm_rdma_send_rkey(id, addr, length, mr);
    uint32_t received_rkey = ktm_rdma_get_rkey(id, addr, length, mr);
    std::memcpy(remote_rkey, &received_rkey, sizeof(uint32_t));
}

void ktm_server_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey) {
    uintptr_t received_addr = ktm_rdma_get_address(id, addr, length, mr);
    std::memcpy(remote_addr, &received_addr, sizeof(uintptr_t));
    ktm_rdma_send_address(id, addr, length, mr);
    uint32_t received_rkey = ktm_rdma_get_rkey(id, addr, length, mr);
    std::memcpy(remote_rkey, &received_rkey, sizeof(uint32_t));
    ktm_rdma_send_rkey(id, addr, length, mr);
}

void ktm_client_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey, struct ibv_mr *rdma_mr) {
    ktm_rdma_send_address(id, addr, length, mr, rdma_mr);
    uintptr_t received_addr = ktm_rdma_get_address(id, addr, length, mr);
    std::memcpy(remote_addr, &received_addr, sizeof(uintptr_t));
    ktm_rdma_send_rkey(id, addr, length, mr, rdma_mr);
    uint32_t received_rkey = ktm_rdma_get_rkey(id, addr, length, mr);
    std::memcpy(remote_rkey, &received_rkey, sizeof(uint32_t));
}

void ktm_server_exchange_rdma_info(struct rdma_cm_id *id, void *addr, size_t length, struct ibv_mr *mr, uintptr_t * remote_addr, uint32_t * remote_rkey, struct ibv_mr *rdma_mr) {
    uintptr_t received_addr = ktm_rdma_get_address(id, addr, length, mr);
    std::memcpy(remote_addr, &received_addr, sizeof(uintptr_t));
    ktm_rdma_send_address(id, addr, length, mr, rdma_mr);
    uint32_t received_rkey = ktm_rdma_get_rkey(id, addr, length, mr);
    std::memcpy(remote_rkey, &received_rkey, sizeof(uint32_t));
    ktm_rdma_send_rkey(id, addr, length, mr, rdma_mr);
}

// ---- Inline helpers (NO per-post query) --------------------------------------

uint32_t ktm_rdma_query_max_inline(struct rdma_cm_id *id) {
    ibv_qp_attr attr{};          // ignored fields will be left zeroed
    ibv_qp_init_attr init_attr{};
    int rc = ibv_query_qp(id->qp, &attr, IBV_QP_CAP, &init_attr);
    if (rc) throw std::runtime_error("ibv_query_qp(IBV_QP_CAP) rc=" + std::to_string(rc));

    // Prefer init_attr.cap if non-zero; otherwise fall back to attr.cap
    uint32_t from_init = init_attr.cap.max_inline_data;
    uint32_t from_attr = attr.cap.max_inline_data;
    return from_init ? from_init : from_attr;
}

void ktm_rdma_post_send_inline_cached(struct rdma_cm_id *id,
                                      void *context,
                                      const void *addr,
                                      size_t length,
                                      int flags,
                                      uint32_t max_inline) {
    if (length > max_inline) {
        throw std::runtime_error("INLINE length " + std::to_string(length) +
                                 " exceeds cached max_inline " + std::to_string(max_inline));
    }

    ibv_sge sge{};
    if (length > 0) {
        sge.addr   = reinterpret_cast<uintptr_t>(addr);
        sge.length = static_cast<uint32_t>(length);
        sge.lkey   = 0; // not used for INLINE
    }

    ibv_send_wr wr{};
    wr.wr_id      = reinterpret_cast<uint64_t>(context);
    wr.next       = nullptr;
    wr.sg_list    = (length > 0) ? &sge : nullptr;
    wr.num_sge    = (length > 0) ? 1 : 0;
    wr.opcode     = IBV_WR_SEND;
    wr.send_flags = flags | ((length > 0) ? IBV_SEND_INLINE : 0);

    ibv_send_wr *bad = nullptr;
    int rc = ibv_post_send(id->qp, &wr, &bad);
    if (rc) {
        throw std::runtime_error("ibv_post_send(INLINE) rc=" + std::to_string(rc));
    }
}
