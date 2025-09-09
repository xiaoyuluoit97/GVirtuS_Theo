//
// Created by Mariano Aponte on 07/12/23.
//

#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>     // posix_memalign/free
#include <arpa/inet.h>
#include <stdexcept>
#include <cerrno>      // NEW: for errno in logs
#include <algorithm>   // NEW: for std::min

#include "RdmaCommunicator.h"

#include <gvirtus/communicators/Endpoint.h>
#include <gvirtus/communicators/Endpoint_Tcp.h>
#include <gvirtus/communicators/Endpoint_Rdma.h>

using gvirtus::communicators::RdmaCommunicator;

// ---------- Helper ------------------------------------------------------------

// NEW: query once and cache QP inline capability
void RdmaCommunicator::cache_inline_cap_() {
    inline_max_ = 0;
    if (!rdmaCmId || !rdmaCmId->qp) {
#ifdef DEBUG
        std::cerr << "cache_inline_cap_: rdmaCmId/qp not ready.\n";
#endif
        inline_enabled = false;
        return;
    }
    ibv_qp_attr attr{};
    ibv_qp_init_attr init{};
    int rc = ibv_query_qp(rdmaCmId->qp, &attr, IBV_QP_CAP, &init);
    if (rc == 0) {
        inline_max_ = attr.cap.max_inline_data;
    } else {
#ifdef DEBUG
        std::cerr << "ibv_query_qp(IBV_QP_CAP) failed, rc=" << rc
                  << " errno=" << errno << " (" << std::strerror(errno) << ")\n";
#endif
        inline_max_ = 0;
    }
    inline_enabled = (inline_max_ > 0);
#ifdef DEBUG
    std::cout << "Cached QP inline cap = " << inline_max_ << " bytes, inline_enabled="
              << inline_enabled << "\n";
#endif
}

void RdmaCommunicator::request_min_rnr_timer_(uint8_t v) {
    if (!rdmaCmId || !rdmaCmId->qp) return;
    ibv_qp_attr qp_attr{};
    qp_attr.min_rnr_timer = v;
    if (ibv_modify_qp(rdmaCmId->qp, &qp_attr, IBV_QP_MIN_RNR_TIMER)) {
#ifdef DEBUG
        std::perror("ibv_modify_qp(IBV_QP_MIN_RNR_TIMER)");
#endif
    }
}

inline char* RdmaCommunicator::pool_reserve_tx_(size_t size, size_t &off) {
    if (size > kPoolSize || !tx_pool_) return nullptr;
    size_t cur = tx_head_ % kPoolSize;
    if (cur + size > kPoolSize) {
        // wrap
        cur = 0;
        tx_head_ = (tx_head_ / kPoolSize + 1) * kPoolSize;
    }
    off = cur;
    return tx_pool_ + cur;
}

inline char* RdmaCommunicator::pool_reserve_rx_(size_t size, size_t &off) {
    if (size > kPoolSize || !rx_pool_) return nullptr;
    size_t cur = rx_head_ % kPoolSize;
    if (cur + size > kPoolSize) {
        // wrap
        cur = 0;
        rx_head_ = (rx_head_ / kPoolSize + 1) * kPoolSize;
    }
    off = cur;
    return rx_pool_ + cur;
}

void RdmaCommunicator::init_pools_() {
    // Already initialized?
    if (tx_pool_ || rx_pool_) return;
    // Allocate page-aligned memory
    void* p = nullptr;
    if (posix_memalign(&p, kPoolAlign, kPoolSize) != 0) {
        throw std::runtime_error("posix_memalign(tx_pool) failed");
    }
    tx_pool_ = static_cast<char*>(p);
    tx_mr_ = ktm_rdma_reg_msgs(rdmaCmId, tx_pool_, kPoolSize);

    p = nullptr;
    if (posix_memalign(&p, kPoolAlign, kPoolSize) != 0) {
        throw std::runtime_error("posix_memalign(rx_pool) failed");
    }
    rx_pool_ = static_cast<char*>(p);
    rx_mr_ = ktm_rdma_reg_msgs(rdmaCmId, rx_pool_, kPoolSize);

    tx_head_ = 0;
    rx_head_ = 0;

#ifdef DEBUG
    std::cout << "Initialized TX/RX pools: 64MB each, lkeys {"
              << (tx_mr_ ? tx_mr_->lkey : 0) << ", "
              << (rx_mr_ ? rx_mr_->lkey : 0) << "}\n";
#endif
}

void RdmaCommunicator::destroy_pools_() {
    if (tx_mr_) { ibv_dereg_mr(tx_mr_); tx_mr_ = nullptr; }
    if (tx_pool_) { std::free(tx_pool_); tx_pool_ = nullptr; }
    if (rx_mr_) { ibv_dereg_mr(rx_mr_); rx_mr_ = nullptr; }
    if (rx_pool_) { std::free(rx_pool_); rx_pool_ = nullptr; }
}

// ---------- Constructors / Destructor -----------------------------------------

RdmaCommunicator::RdmaCommunicator(const std::string& hostname, const std::string& port, bool isRoce)
    : isRoce(isRoce) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(" << hostname << ", " << port << ", isRoce=" << isRoce << ")\n";
#endif

    if (port.empty()) throw std::runtime_error("RdmaCommunicator: Port not specified...");

    hostent *ent = gethostbyname(hostname.c_str());
    if (ent == nullptr) {
        std::ostringstream oss;
        oss << "RdmaCommunicator: Can't resolve hostname \"" << hostname << "\"...";
        throw std::runtime_error(oss.str());
    }

    std::strncpy(this->hostname, hostname.c_str(), sizeof(this->hostname) - 1);
    std::strncpy(this->port,     port.c_str(),     sizeof(this->port) - 1);

    rdmaCmId = nullptr;
    rdmaCmListenId = nullptr;
    memoryRegion = nullptr;
    preregisteredMr = nullptr;
    inline_enabled = true;
    inline_max_ = 0; // NEW: init

    tx_pool_ = rx_pool_ = nullptr;
    tx_mr_ = rx_mr_ = nullptr;
    tx_head_ = rx_head_ = 0;
}

// Constructor used on the server side when a connection is accepted
RdmaCommunicator::RdmaCommunicator(rdma_cm_id *rdmaCmId)
    : isRoce(false) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(rdma_cm_id *rdmaCmId)\n";
#endif
    if (!rdmaCmId || !rdmaCmId->qp) throw std::runtime_error("Accepted rdma_cm_id is null or has no QP");
    this->rdmaCmId = rdmaCmId;

    // Cache inline capability once (server side)
    cache_inline_cap_(); // NEW

    // Pre-register small bounce buffer
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);

    // Init TX/RX pools (64MB each)
    init_pools_();
}

RdmaCommunicator::~RdmaCommunicator() {
#ifdef DEBUG
    std::cout << "Called ~RdmaCommunicator()\n";
#endif
    destroy_pools_();

    if (preregisteredMr) { ibv_dereg_mr(preregisteredMr); preregisteredMr = nullptr; }
    if (memoryRegion)    { ibv_dereg_mr(memoryRegion);    memoryRegion    = nullptr; }
    if (rdmaCmId) {
        rdma_disconnect(rdmaCmId);
        rdma_destroy_id(rdmaCmId);
        rdmaCmId = nullptr;
    }
    if (rdmaCmListenId) {
        rdma_destroy_id(rdmaCmListenId);
        rdmaCmListenId = nullptr;
    }
}

// ---------- Passive (server) path ---------------------------------------------

void RdmaCommunicator::Serve() {
#ifdef DEBUG
    std::cout << "Called Serve()\n";
#endif

    rdma_addrinfo hints{};
    hints.ai_port_space = isRoce ? RDMA_PS_TCP : RDMA_PS_IB;
    hints.ai_flags = RAI_PASSIVE;

    rdma_addrinfo *rdmaAddrinfo = nullptr;
    ktm_rdma_getaddrinfo(this->hostname, this->port, &hints, &rdmaAddrinfo);

    ibv_qp_init_attr qpInitAttr{};
    qpInitAttr.cap.max_send_wr      = 256;
    qpInitAttr.cap.max_recv_wr      = 256;
    qpInitAttr.cap.max_send_sge     = 4;
    qpInitAttr.cap.max_recv_sge     = 4;
    qpInitAttr.cap.max_inline_data  = 120; // request some inline capability
    qpInitAttr.sq_sig_all           = 1;
    qpInitAttr.qp_type              = IBV_QPT_RC;

    ktm_rdma_create_ep(&rdmaCmListenId, rdmaAddrinfo, nullptr, &qpInitAttr);
    rdma_freeaddrinfo(rdmaAddrinfo);

    ktm_rdma_listen(rdmaCmListenId, BACKLOG);
}

const gvirtus::communicators::Communicator *const RdmaCommunicator::Accept() const {
#ifdef DEBUG
    std::cout << "Called Accept()\n";
#endif
    rdma_cm_id *clientRdmaCmId = nullptr;
    ktm_rdma_get_request(rdmaCmListenId, &clientRdmaCmId);
    ktm_rdma_accept(clientRdmaCmId, nullptr);

    // Best-effort RNR tweak (optional)
    auto *ibvQpAttr = static_cast<ibv_qp_attr *>(std::malloc(sizeof(ibv_qp_attr)));
    if (ibvQpAttr) {
        std::memset(ibvQpAttr, 0, sizeof(*ibvQpAttr));
        ibvQpAttr->min_rnr_timer = 1;
        if (ibv_modify_qp(clientRdmaCmId->qp, ibvQpAttr, IBV_QP_MIN_RNR_TIMER)) {
            std::fprintf(stderr, "ibv_modify_qp(IBV_QP_MIN_RNR_TIMER) failed: %s\n", std::strerror(errno));
        }
        std::free(ibvQpAttr);
    }

    // New communicator will cache inline cap & init pools
    return new RdmaCommunicator(clientRdmaCmId);
}

// ---------- Active (client) path ----------------------------------------------

void RdmaCommunicator::Connect() {
#ifdef DEBUG
    std::cout << "Called Connect()\n";
#endif

    rdma_addrinfo hints{};
    hints.ai_family    = AF_INET;
    hints.ai_port_space= isRoce ? RDMA_PS_TCP : RDMA_PS_IB;

    rdma_addrinfo *rdmaAddrinfo = nullptr;
    ktm_rdma_getaddrinfo(this->hostname, this->port, &hints, &rdmaAddrinfo);

    ibv_qp_init_attr qpInitAttr{};
    qpInitAttr.cap.max_send_wr      = 256;
    qpInitAttr.cap.max_recv_wr      = 256;
    qpInitAttr.cap.max_send_sge     = 4;
    qpInitAttr.cap.max_recv_sge     = 4;
    qpInitAttr.cap.max_inline_data  = 120; // request inline capability
    qpInitAttr.sq_sig_all           = 1;
    qpInitAttr.qp_type              = IBV_QPT_RC;

    ktm_rdma_create_ep(&rdmaCmId, rdmaAddrinfo, nullptr, &qpInitAttr);
    rdma_freeaddrinfo(rdmaAddrinfo);

    ktm_rdma_connect(rdmaCmId, nullptr);

    request_min_rnr_timer_(1);

    // Cache inline capability once (client side)
    cache_inline_cap_(); // NEW

    // Pre-register the small bounce buffer once
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);

    // Init TX/RX pools (64MB each)
    init_pools_();
}

// ---------- Data plane ---------------------------------------------------------

size_t RdmaCommunicator::Read(char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Read(char *buffer, size_t size) - Size: " << size << std::endl;
#endif

    if (size < kSmallThreshold) {
        // Use small preregistered bounce buffer
        ktm_rdma_post_recv(rdmaCmId, nullptr, preregisteredBuffer, size, preregisteredMr);
    } else {
        // Use RX pool (no per-call reg/dereg)
        size_t off = 0;
        char* dst = pool_reserve_rx_(size, off);
        if (!dst) {
            // Fallback: extremely large read (>64MB) — old path (register user buffer just for this call)
            memoryRegion = ktm_rdma_reg_msgs(rdmaCmId, buffer, size);
            ktm_rdma_post_recv(rdmaCmId, nullptr, buffer, size, memoryRegion);
        } else {
            ktm_rdma_post_recv(rdmaCmId, nullptr, dst, size, rx_mr_);
        }
    }

    int num_comp;
    do num_comp = ibv_poll_cq(rdmaCmId->recv_cq, 1, &workCompletion); while (num_comp == 0);
    if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(recv) failed");
    if (workCompletion.status != IBV_WC_SUCCESS)
        throw std::runtime_error(std::string("RECV failed status ") + ibv_wc_status_str(workCompletion.status));

    if (size < kSmallThreshold) {
        std::memcpy(buffer, preregisteredBuffer, size);
    } else {
        // If we used pool, copy from pool to user; else (fallback) dereg MR.
        if (memoryRegion) {
            ibv_dereg_mr(memoryRegion);
            memoryRegion = nullptr;
        } else {
            size_t cur = rx_head_ % kPoolSize;
            char* src = rx_pool_ + cur;
            std::memcpy(buffer, src, size);
            // Advance ring head
            rx_head_ += size;
        }
    }

    return size;
}

size_t RdmaCommunicator::Write(const char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Write(const char *buffer, size_t size) - Size: " << size << std::endl;
#endif

    // 1) Try INLINE if enabled and payload fits the cached threshold (cap).
    //    Use min(magic, inline_max_) so你仍可用100B“保守上限”限制CPU copy，但不会超过设备能力。
    if (inline_enabled) {
        size_t inline_threshold = std::min<size_t>(kInlineMagicBytes, inline_max_); // NEW
        if (size <= inline_threshold) {
            ibv_sge sge{};
            if (size > 0) {
                sge.addr   = reinterpret_cast<uintptr_t>(buffer);
                sge.length = static_cast<uint32_t>(size);
                sge.lkey   = 0; // ignored for INLINE
            }

            ibv_send_wr wr{};
            wr.wr_id      = 0;
            wr.next       = nullptr;
            wr.sg_list    = (size > 0) ? &sge : nullptr;
            wr.num_sge    = (size > 0) ? 1 : 0;
            wr.opcode     = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED | ((size > 0) ? IBV_SEND_INLINE : 0);

            ibv_send_wr *bad = nullptr;
            int rc = ibv_post_send(rdmaCmId->qp, &wr, &bad);
            if (rc == 0) {
                int num_comp;
                do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
                if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(send, inline) failed");
                if (workCompletion.status != IBV_WC_SUCCESS)
                    throw std::runtime_error(std::string("SEND (inline) failed status ") + ibv_wc_status_str(workCompletion.status));
                return size;
            } else {
#ifdef DEBUG
                std::cerr << "INLINE post failed (rc=" << rc << ", errno=" << errno
                          << " " << std::strerror(errno)
                          << "). Falling back to non-inline for this send only.\n";
#endif
                // 注意：不要关闭 inline_enabled，可能是瞬时资源问题。
                // 直接跌落到非-inline路径处理本次发送。
            }
        }
    }

    // 2) Non-inline paths.
    if (size < kSmallThreshold) {
        // Small non-inline: use preregistered bounce
        std::memcpy(preregisteredBuffer, buffer, size);
        ktm_rdma_post_send(rdmaCmId, nullptr, preregisteredBuffer, size, preregisteredMr, IBV_SEND_SIGNALED);

        int num_comp;
        do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
        if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(send, small) failed");
        if (workCompletion.status != IBV_WC_SUCCESS)
            throw std::runtime_error(std::string("SEND (small) failed status ") + ibv_wc_status_str(workCompletion.status));
        return size;
    } else {
        // Large: use TX pool (no reg/dereg)
        size_t off = 0;
        char* dst = pool_reserve_tx_(size, off);
        if (!dst) {
            // Extremely large (>64MB): fallback old path (register caller buffer once)
            ibv_mr *mr = ktm_rdma_reg_msgs(rdmaCmId, const_cast<char*>(buffer), size);
            ktm_rdma_post_send(rdmaCmId, nullptr, const_cast<char*>(buffer), size, mr, IBV_SEND_SIGNALED);

            int num_comp;
            do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
            if (num_comp < 0) { ibv_dereg_mr(mr); throw std::runtime_error("ibv_poll_cq(send, large-fallback) failed"); }
            if (workCompletion.status != IBV_WC_SUCCESS) {
                ibv_dereg_mr(mr);
                throw std::runtime_error(std::string("SEND (large-fallback) failed status ") + ibv_wc_status_str(workCompletion.status));
            }
            ibv_dereg_mr(mr);
            return size;
        }

        // Copy to TX pool and post
        std::memcpy(dst, buffer, size);

        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uintptr_t>(dst);
        sge.length = static_cast<uint32_t>(size);
        sge.lkey   = tx_mr_->lkey;

        ibv_send_wr wr{};
        wr.wr_id      = 0;
        wr.next       = nullptr;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;

        ibv_send_wr *bad = nullptr;
        int rc = ibv_post_send(rdmaCmId->qp, &wr, &bad);
        if (rc) throw std::runtime_error("ibv_post_send(tx_pool) rc=" + std::to_string(rc));

        int num_comp;
        do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
        if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(send, tx_pool) failed");
        if (workCompletion.status != IBV_WC_SUCCESS)
            throw std::runtime_error(std::string("SEND (tx_pool) failed status ") + ibv_wc_status_str(workCompletion.status));

        // Advance ring head only after completion (同步 API，安全复用)
        tx_head_ += size;
        return size;
    }
}

void RdmaCommunicator::Sync() {
#ifdef DEBUG
    std::cout << "RdmaCommunicator::Sync(): called.\n";
#endif
}

void RdmaCommunicator::Close() {
#ifdef DEBUG
    std::cout << "RdmaCommunicator::Close(): called.\n";
#endif
    destroy_pools_();
    if (preregisteredMr) { ibv_dereg_mr(preregisteredMr); preregisteredMr = nullptr; }
    if (memoryRegion)    { ibv_dereg_mr(memoryRegion);    memoryRegion    = nullptr; }
    if (rdmaCmId) {
        rdma_disconnect(rdmaCmId);
        rdma_destroy_id(rdmaCmId);
        rdmaCmId = nullptr;
    }
}

// ---------- Factory function ---------------------------------------------------

extern "C" std::shared_ptr<RdmaCommunicator> create_communicator(std::shared_ptr<gvirtus::communicators::Endpoint> end) {
    std::string hostname = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->address();
    std::string port = std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->port());

    bool isRoce = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->suite() == "roce-rdma";

    return std::make_shared<RdmaCommunicator>(hostname, port, isRoce);
}
