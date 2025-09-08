//
// Created by Mariano Aponte on 07/12/23.
//

#include <iostream>
#include <sstream>
#include <cstring>
#include <arpa/inet.h>
#include <stdexcept>

#include "RdmaCommunicator.h"

#include <gvirtus/communicators/Endpoint.h>
#include <gvirtus/communicators/Endpoint_Tcp.h>
#include <gvirtus/communicators/Endpoint_Rdma.h>

using gvirtus::communicators::RdmaCommunicator;

// ---------- Internal helpers (private) ----------------------------------------

/**
 * @brief Query QP capability once and cache max_inline_data.
 *        Looks at init_attr.cap.max_inline_data primarily, then attr.cap as fallback.
 */
void RdmaCommunicator::cache_max_inline_() {
    if (!rdmaCmId || !rdmaCmId->qp)
        throw std::runtime_error("cache_max_inline_(): no QP available");

    ibv_qp_attr attr{};
    ibv_qp_init_attr init_attr{};
    int rc = ibv_query_qp(rdmaCmId->qp, &attr, IBV_QP_CAP, &init_attr);
    if (rc) {
        throw std::runtime_error("ibv_query_qp(IBV_QP_CAP) failed, rc=" + std::to_string(rc));
    }

    uint32_t from_init = init_attr.cap.max_inline_data;
    uint32_t from_attr = attr.cap.max_inline_data;
    max_inline_data = from_init ? from_init : from_attr;

#ifdef DEBUG
    std::cout << "Cached max_inline_data = " << max_inline_data << " bytes" << std::endl;
#endif
}

/**
 * @brief Best-effort min_rnr_timer tweak (safe no-op if it fails).
 */
void RdmaCommunicator::request_min_rnr_timer_(uint8_t v) {
    if (!rdmaCmId || !rdmaCmId->qp) return;
    ibv_qp_attr qp_attr{};
    qp_attr.min_rnr_timer = v;
    if (ibv_modify_qp(rdmaCmId->qp, &qp_attr, IBV_QP_MIN_RNR_TIMER)) {
        // Non-fatal; keep running.
#ifdef DEBUG
        std::perror("ibv_modify_qp(IBV_QP_MIN_RNR_TIMER)");
#endif
    }
}

// ---------- Constructors / Destructor -----------------------------------------

RdmaCommunicator::RdmaCommunicator(const std::string& host, const std::string& prt, bool roce)
    : isRoce(roce) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(" << host << ", " << prt << ", isRoce=" << isRoce << ")" << std::endl;
#endif
    if (prt.empty()) {
        throw std::runtime_error("RdmaCommunicator: Port not specified...");
    }

    hostent *ent = gethostbyname(host.c_str());
    if (ent == nullptr) {
        std::ostringstream oss;
        oss << "RdmaCommunicator: Can't resolve hostname \"" << host << "\"...";
        throw std::runtime_error(oss.str());
    }

    std::strncpy(this->hostname, host.c_str(), sizeof(this->hostname) - 1);
    std::strncpy(this->port,     prt.c_str(),  sizeof(this->port) - 1);

    rdmaCmId = nullptr;
    rdmaCmListenId = nullptr;
    memoryRegion = nullptr;
    preregisteredMr = nullptr;
    max_inline_data = 0;
}

RdmaCommunicator::RdmaCommunicator(rdma_cm_id *id)
    : isRoce(false) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(rdma_cm_id *rdmaCmId)" << std::endl;
#endif
    if (!id || !id->qp) throw std::runtime_error("RdmaCommunicator: accepted id or QP is null");
    rdmaCmId = id;

    // Cache inline capability for this QP and pre-register the small bounce buffer.
    cache_max_inline_();
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);
}

RdmaCommunicator::~RdmaCommunicator() {
#ifdef DEBUG
    std::cout << "Called ~RdmaCommunicator()" << std::endl;
#endif
    // Deregister MRs if present
    if (preregisteredMr) {
        ibv_dereg_mr(preregisteredMr);
        preregisteredMr = nullptr;
    }
    if (memoryRegion) {
        ibv_dereg_mr(memoryRegion);
        memoryRegion = nullptr;
    }

    // Best-effort disconnect and destroy IDs
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
    std::cout << "Called Serve()" << std::endl;
#endif
    rdma_addrinfo hints{};
    // Select RDMA port space depending on isRoce flag
    hints.ai_port_space = isRoce ? RDMA_PS_TCP : RDMA_PS_IB;
    hints.ai_flags = RAI_PASSIVE;

    rdma_addrinfo *rdmaAddrinfo = nullptr;
    ktm_rdma_getaddrinfo(this->hostname, this->port, &hints, &rdmaAddrinfo);

    ibv_qp_init_attr qpInitAttr{};
    qpInitAttr.cap.max_send_wr      = 256;
    qpInitAttr.cap.max_recv_wr      = 256;
    qpInitAttr.cap.max_send_sge     = 4;
    qpInitAttr.cap.max_recv_sge     = 4;
    qpInitAttr.cap.max_inline_data  = 120; // request some inline capability up front
    qpInitAttr.sq_sig_all           = 1;
    qpInitAttr.qp_type              = IBV_QPT_RC;

    ktm_rdma_create_ep(&rdmaCmListenId, rdmaAddrinfo, nullptr, &qpInitAttr);
    rdma_freeaddrinfo(rdmaAddrinfo);

    ktm_rdma_listen(rdmaCmListenId, BACKLOG);
}

const gvirtus::communicators::Communicator *const RdmaCommunicator::Accept() const {
#ifdef DEBUG
    std::cout << "Called Accept()" << std::endl;
#endif
    rdma_cm_id *clientRdmaCmId = nullptr;
    ktm_rdma_get_request(rdmaCmListenId, &clientRdmaCmId);
    ktm_rdma_accept(clientRdmaCmId, nullptr);

    // Best-effort min_rnr tweak on the accepted QP (not critical)
    auto *ibvQpAttr = static_cast<ibv_qp_attr *>(std::malloc(sizeof(ibv_qp_attr)));
    if (ibvQpAttr) {
        std::memset(ibvQpAttr, 0, sizeof(*ibvQpAttr));
        ibvQpAttr->min_rnr_timer = 1;
        if (ibv_modify_qp(clientRdmaCmId->qp, ibvQpAttr, IBV_QP_MIN_RNR_TIMER)) {
            std::fprintf(stderr, "ibv_modify_qp(IBV_QP_MIN_RNR_TIMER) failed: %s\n", std::strerror(errno));
        }
        std::free(ibvQpAttr);
    }

    // Build a new communicator bound to this accepted connection.
    // Its constructor will cache inline capability and preregister the small buffer.
    return new RdmaCommunicator(clientRdmaCmId);
}

// ---------- Active (client) path ----------------------------------------------

void RdmaCommunicator::Connect() {
#ifdef DEBUG
    std::cout << "Called Connect()" << std::endl;
#endif
    rdma_addrinfo hints{};
    std::memset(&hints, 0, sizeof(hints));
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

    // Optional tuning
    request_min_rnr_timer_(1);

    // Cache inline capability and preregister the bounce buffer once.
    cache_max_inline_();
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);
}

// ---------- Data plane ---------------------------------------------------------

size_t RdmaCommunicator::Read(char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Read(char *buffer, size_t size) - Size: " << size << std::endl;
#endif
    bool used_temp_mr = false;

    if (size < kSmallThreshold) {
        // Post RECV into our pre-registered bounce buffer
        ktm_rdma_post_recv(rdmaCmId, nullptr, preregisteredBuffer, size, preregisteredMr);
    } else {
        // Register user buffer for large receive (one-shot)
        memoryRegion = ktm_rdma_reg_msgs(rdmaCmId, buffer, size);
        used_temp_mr = true;
        ktm_rdma_post_recv(rdmaCmId, nullptr, buffer, size, memoryRegion);
    }

    int num_comp;
    do num_comp = ibv_poll_cq(rdmaCmId->recv_cq, 1, &workCompletion); while (num_comp == 0);
    if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(recv) failed");
    if (workCompletion.status != IBV_WC_SUCCESS)
        throw std::runtime_error(std::string("RECV failed status ") + ibv_wc_status_str(workCompletion.status));

    if (size < kSmallThreshold) {
        std::memcpy(buffer, preregisteredBuffer, size);
    } else if (used_temp_mr) {
        // Deregister the one-shot MR after completion
        ibv_dereg_mr(memoryRegion);
        memoryRegion = nullptr;
    }

    return size;
}

size_t RdmaCommunicator::Write(const char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Write(const char *buffer, size_t size) - Size: " << size << std::endl;
#endif
    // 1) Fast path: INLINE if supported and length fits.
    if (max_inline_data > 0 && size <= max_inline_data) {
        ibv_sge sge{}; // will be ignored by HCA for INLINE, but many providers still expect it if length>0
        if (size > 0) {
            sge.addr   = reinterpret_cast<uintptr_t>(buffer);
            sge.length = static_cast<uint32_t>(size);
            sge.lkey   = 0; // not used for INLINE
        }

        ibv_send_wr wr{};
        wr.wr_id      = 0; // caller context not used here; adapt if you need it in CQE
        wr.next       = nullptr;
        wr.sg_list    = (size > 0) ? &sge : nullptr;
        wr.num_sge    = (size > 0) ? 1 : 0;
        wr.opcode     = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED | ((size > 0) ? IBV_SEND_INLINE : 0);

        ibv_send_wr *bad = nullptr;
        int rc = ibv_post_send(rdmaCmId->qp, &wr, &bad);
        if (rc) {
            throw std::runtime_error("ibv_post_send(INLINE) rc=" + std::to_string(rc));
        }

        int num_comp;
        do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
        if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(send, inline) failed");
        if (workCompletion.status != IBV_WC_SUCCESS)
            throw std::runtime_error(std::string("SEND (inline) failed status ") + ibv_wc_status_str(workCompletion.status));

        return size;
    }

    // 2) Otherwise: use pre-registered bounce buffer for small non-inline,
    //    or register-on-the-fly for large payloads.
    if (size < kSmallThreshold) {
        std::memcpy(preregisteredBuffer, buffer, size);
        ktm_rdma_post_send(rdmaCmId, nullptr, preregisteredBuffer, size, preregisteredMr, IBV_SEND_SIGNALED);
        int num_comp;
        do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
        if (num_comp < 0) throw std::runtime_error("ibv_poll_cq(send, small) failed");
        if (workCompletion.status != IBV_WC_SUCCESS)
            throw std::runtime_error(std::string("SEND (small) failed status ") + ibv_wc_status_str(workCompletion.status));
        return size;
    } else {
        // Large send: register and post directly from the caller buffer (avoids extra copy)
        ibv_mr *mr = ktm_rdma_reg_msgs(rdmaCmId, const_cast<char*>(buffer), size);
        ktm_rdma_post_send(rdmaCmId, nullptr, const_cast<char*>(buffer), size, mr, IBV_SEND_SIGNALED);

        int num_comp;
        do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
        if (num_comp < 0) { ibv_dereg_mr(mr); throw std::runtime_error("ibv_poll_cq(send, large) failed"); }
        if (workCompletion.status != IBV_WC_SUCCESS) {
            ibv_dereg_mr(mr);
            throw std::runtime_error(std::string("SEND (large) failed status ") + ibv_wc_status_str(workCompletion.status));
        }

        ibv_dereg_mr(mr);
        return size;
    }
}

void RdmaCommunicator::Sync() {
#ifdef DEBUG
    std::cout << "RdmaCommunicator::Sync(): called." << std::endl;
#endif
}

void RdmaCommunicator::Close() {
#ifdef DEBUG
    std::cout << "RdmaCommunicator::Close(): called." << std::endl;
#endif
    if (preregisteredMr) { ibv_dereg_mr(preregisteredMr); preregisteredMr = nullptr; }
    if (memoryRegion)    { ibv_dereg_mr(memoryRegion);    memoryRegion    = nullptr; }
    if (rdmaCmId) {
        rdma_disconnect(rdmaCmId);
        rdma_destroy_id(rdmaCmId);
        rdmaCmId = nullptr;
    }
}

// ---------- Factory (kept as-is) ----------------------------------------------

extern "C" std::shared_ptr<RdmaCommunicator> create_communicator(std::shared_ptr<gvirtus::communicators::Endpoint> end) {
    std::string hostname = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->address();
    std::string port     = std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->port());

    // Determine if this is a RoCE endpoint
    bool isRoce = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->suite() == "roce-rdma";

    return std::make_shared<RdmaCommunicator>(hostname, port, isRoce);
}
