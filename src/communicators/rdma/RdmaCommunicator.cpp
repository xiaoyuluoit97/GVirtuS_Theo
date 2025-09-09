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

// ---------- Helper ------------------------------------------------------------

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
}

// Constructor used on the server side when a connection is accepted
RdmaCommunicator::RdmaCommunicator(rdma_cm_id *rdmaCmId)
    : isRoce(false) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(rdma_cm_id *rdmaCmId)\n";
#endif
    if (!rdmaCmId || !rdmaCmId->qp) throw std::runtime_error("Accepted rdma_cm_id is null or has no QP");
    this->rdmaCmId = rdmaCmId;

    // Pre-register small bounce buffer
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);
    inline_enabled = true; // optimistic: we requested inline capability at QP creation
}

RdmaCommunicator::~RdmaCommunicator() {
#ifdef DEBUG
    std::cout << "Called ~RdmaCommunicator()\n";
#endif
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
    qpInitAttr.cap.max_inline_data  = 120; // request some inline capability (cheap, control-path)
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
    qpInitAttr.cap.max_inline_data  = 120; // request inline capability; we won't query the actual limit
    qpInitAttr.sq_sig_all           = 1;
    qpInitAttr.qp_type              = IBV_QPT_RC;

    ktm_rdma_create_ep(&rdmaCmId, rdmaAddrinfo, nullptr, &qpInitAttr);
    rdma_freeaddrinfo(rdmaAddrinfo);

    ktm_rdma_connect(rdmaCmId, nullptr);

    request_min_rnr_timer_(1);

    // Pre-register the small bounce buffer once
    preregisteredMr = ktm_rdma_reg_msgs(rdmaCmId, preregisteredBuffer, kSmallThreshold);
    inline_enabled = true; // optimistic enabling; will auto-disable on first failure
}

// ---------- Data plane ---------------------------------------------------------

size_t RdmaCommunicator::Read(char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Read(char *buffer, size_t size) - Size: " << size << std::endl;
#endif

    bool used_temp_mr = false;
    if (size < kSmallThreshold) {
        ktm_rdma_post_recv(rdmaCmId, nullptr, preregisteredBuffer, size, preregisteredMr);
    } else {
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
        ibv_dereg_mr(memoryRegion);
        memoryRegion = nullptr;
    }

    return size;
}

size_t RdmaCommunicator::Write(const char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Write(const char *buffer, size_t size) - Size: " << size << std::endl;
#endif

    // 1) Try INLINE if enabled and payload fits the magic threshold.
    if (inline_enabled && size <= kInlineMagicBytes) {
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
            // Provider rejected INLINE (e.g., no support or too small limit) — permanently disable and fall through.
#ifdef DEBUG
            std::cerr << "INLINE post rejected (rc=" << rc << "). Disabling inline path.\n";
#endif
            inline_enabled = false;
            // fall through to non-inline path for this send
        }
    }

    // 2) Non-inline paths: preregistered bounce buffer for small, register-on-the-fly for large.
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
    std::cout << "RdmaCommunicator::Sync(): called.\n";
#endif
}

void RdmaCommunicator::Close() {
#ifdef DEBUG
    std::cout << "RdmaCommunicator::Close(): called.\n";
#endif
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
