//
// Created by Mariano Aponte on 07/12/23.
//

#include <iostream>
#include <sstream>
#include <cstring>
#include <arpa/inet.h>

#include "RdmaCommunicator.h"

#include <gvirtus/communicators/Endpoint.h>
#include <gvirtus/communicators/Endpoint_Tcp.h>
#include <gvirtus/communicators/Endpoint_Rdma.h>

// Use ktm helpers (includes ktm_rdma_post_send_inline)
#include "ktmrdma.h"

using gvirtus::communicators::RdmaCommunicator;

RdmaCommunicator::RdmaCommunicator(const std::string& hostname, const std::string& port)
    : RdmaCommunicator(hostname, port, false) {}

RdmaCommunicator::RdmaCommunicator(const std::string& hostname, const std::string& port, bool isRoce)
    : rdmaCmId(nullptr),
      rdmaCmListenId(nullptr),
      workCompletion{},
      memoryRegion(nullptr),
      isRoce(isRoce),
      maxInlineData_(0) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(" << hostname << ", " << port
              << ", isRoce=" << std::boolalpha << isRoce << ")" << std::endl;
#endif

    if (port.empty()) {
        throw std::runtime_error("RdmaCommunicator: Port not specified...");
    }

    hostent *ent = gethostbyname(hostname.c_str());
    if (ent == nullptr) {
        std::ostringstream oss;
        oss << "RdmaCommunicator: Can't resolve hostname \"" << hostname << "\"...";
        throw std::runtime_error(oss.str());
    }

    std::memset(this->hostname, 0, sizeof(this->hostname));
    std::memset(this->port, 0, sizeof(this->port));

    std::strncpy(this->hostname, hostname.c_str(), sizeof(this->hostname) - 1);
    std::strncpy(this->port,     port.c_str(),     sizeof(this->port)     - 1);
}

// Constructor used on the server side when a connection is accepted
RdmaCommunicator::RdmaCommunicator(rdma_cm_id *rdmaCmId)
    : rdmaCmId(rdmaCmId),
      rdmaCmListenId(nullptr),
      workCompletion{},
      memoryRegion(nullptr),
      isRoce(false),
      maxInlineData_(0) {
#ifdef DEBUG
    std::cout << "Called RdmaCommunicator(rdma_cm_id *rdmaCmId)" << std::endl;
#endif
    // Query actual inline capability
    ibv_qp_attr attr;
    ibv_qp_init_attr init_attr;
    std::memset(&attr, 0, sizeof(attr));
    std::memset(&init_attr, 0, sizeof(init_attr));
    if (ibv_query_qp(rdmaCmId->qp, &attr, IBV_QP_CAP, &init_attr) == 0) {
        maxInlineData_ = init_attr.cap.max_inline_data;
#ifdef DEBUG
        std::cout << "maxInlineData_ (server) = " << maxInlineData_ << std::endl;
#endif
    } else {
        maxInlineData_ = 0;
    }
}

RdmaCommunicator::~RdmaCommunicator() {
#ifdef DEBUG
    std::cout << "Called ~RdmaCommunicator()" << std::endl;
#endif
    if (rdmaCmId) {
        rdma_disconnect(rdmaCmId);
        rdma_destroy_id(rdmaCmId);
        rdmaCmId = nullptr;
    }
}

void RdmaCommunicator::Serve() {
#ifdef DEBUG
    std::cout << "Called Serve()" << std::endl;
#endif

    rdma_addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));

    // Select RDMA port space depending on isRoce flag
    hints.ai_port_space = isRoce ? RDMA_PS_TCP : RDMA_PS_IB;
    hints.ai_flags      = RAI_PASSIVE;

    rdma_addrinfo *rdmaAddrinfo;
    ktm_rdma_getaddrinfo(this->hostname, this->port, &hints, &rdmaAddrinfo);

    ibv_qp_init_attr qpInitAttr;
    std::memset(&qpInitAttr, 0, sizeof(qpInitAttr));
    qpInitAttr.cap.max_send_wr  = 10;
    qpInitAttr.cap.max_recv_wr  = 10;
    qpInitAttr.cap.max_send_sge = 10;
    qpInitAttr.cap.max_recv_sge = 10;
    qpInitAttr.sq_sig_all       = 1;
    qpInitAttr.qp_type          = IBV_QPT_RC;

    // Listen endpoint creation; QP will be on the accepted client id
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

    // Configure min_rnr_timer safely
    auto *ibvQpAttr = static_cast<ibv_qp_attr *>(std::malloc(sizeof(ibv_qp_attr)));
    std::memset(ibvQpAttr, 0, sizeof(ibv_qp_attr));
    ibvQpAttr->min_rnr_timer = 1;
    if (ibv_modify_qp(clientRdmaCmId->qp, ibvQpAttr, IBV_QP_MIN_RNR_TIMER)) {
        std::fprintf(stderr, "ibv_modify_qp() failed: %s\n", std::strerror(errno));
    }
    std::free(ibvQpAttr);

    return new RdmaCommunicator(clientRdmaCmId);
}

void RdmaCommunicator::Connect() {
#ifdef DEBUG
    std::cout << "Called Connect()" << std::endl;
#endif

    rdma_addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family     = AF_INET;
    hints.ai_port_space = isRoce ? RDMA_PS_TCP : RDMA_PS_IB;

    rdma_addrinfo *rdmaAddrinfo;
    ktm_rdma_getaddrinfo(this->hostname, this->port, &hints, &rdmaAddrinfo);

    ibv_qp_init_attr qpInitAttr;
    std::memset(&qpInitAttr, 0, sizeof(qpInitAttr));
    qpInitAttr.cap.max_send_wr  = 10;
    qpInitAttr.cap.max_recv_wr  = 10;
    qpInitAttr.cap.max_send_sge = 10;
    qpInitAttr.cap.max_recv_sge = 10;
    qpInitAttr.sq_sig_all       = 1;
    qpInitAttr.qp_type          = IBV_QPT_RC;

    // Request inline capability (device may reduce it)
    qpInitAttr.cap.max_inline_data = 32;

    ktm_rdma_create_ep(&rdmaCmId, rdmaAddrinfo, nullptr, &qpInitAttr);
    rdma_freeaddrinfo(rdmaAddrinfo);

    ktm_rdma_connect(rdmaCmId, nullptr);

    // Configure min_rnr_timer safely
    auto *ibvQpAttr = static_cast<ibv_qp_attr *>(std::malloc(sizeof(ibv_qp_attr)));
    std::memset(ibvQpAttr, 0, sizeof(ibv_qp_attr));
    ibvQpAttr->min_rnr_timer = 1;
    if (ibv_modify_qp(rdmaCmId->qp, ibvQpAttr, IBV_QP_MIN_RNR_TIMER)) {
        std::fprintf(stderr, "ibv_modify_qp() failed: %s\n", std::strerror(errno));
    }
    std::free(ibvQpAttr);

    // Query actual inline capability
    ibv_qp_attr attr;
    ibv_qp_init_attr init_attr;
    std::memset(&attr, 0, sizeof(attr));
    std::memset(&init_attr, 0, sizeof(init_attr));
    if (ibv_query_qp(rdmaCmId->qp, &attr, IBV_QP_CAP, &init_attr) == 0) {
        maxInlineData_ = init_attr.cap.max_inline_data;
#ifdef DEBUG
        std::cout << "maxInlineData_ (client) = " << maxInlineData_ << std::endl;
#endif
    } else {
        maxInlineData_ = 0;
    }
}

size_t RdmaCommunicator::Read(char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "Called Read(char *buffer, size_t size) - Size: " << size << std::endl;
#endif

    // Always register the user buffer and post a receive into it
    memoryRegion = ktm_rdma_reg_msgs(rdmaCmId, buffer, size);
#ifdef DEBUG
std::cout << "[RDMA][Read] posting RECV into user buffer ("
          << size << " bytes)" << std::endl;
#endif

    ktm_rdma_post_recv(rdmaCmId, nullptr, buffer, size, memoryRegion);

    int num_comp;
    do num_comp = ibv_poll_cq(rdmaCmId->recv_cq, 1, &workCompletion); while (num_comp == 0);
    if (num_comp < 0) throw "ibv_poll_cq() failed";
    if (workCompletion.status != IBV_WC_SUCCESS)
        throw std::string("Failed status ") + ibv_wc_status_str(workCompletion.status);

    // Data is already in 'buffer'; no memcpy needed
    return size;
}

size_t RdmaCommunicator::Write(const char *buffer, size_t size) {
#ifdef DEBUG
    std::cout << "[RDMA][Write] requested size=" << size
              << " bytes, maxInlineData_=" << maxInlineData_ << std::endl;
#endif

    char *actualBuffer = nullptr;

    // Prefer inline if supported and size fits
    if (maxInlineData_ > 0 && size <= maxInlineData_) {

#ifdef DEBUG
        std::cout << "[RDMA][Write] USING INLINE SEND ("
                  << size << " bytes)" << std::endl;
#endif

        ktm_rdma_post_send_inline(rdmaCmId, nullptr, buffer, size, IBV_SEND_SIGNALED);
    } else {
#ifdef DEBUG
        std::cout << "[RDMA][Write] USING REGISTERED MR (LARGE SEND, "
                  << size << " bytes)" << std::endl;
#endif
        // Large payload: allocate a temporary buffer, register MR, send
        actualBuffer = static_cast<char *>(std::malloc(size));
        if (!actualBuffer) {
            throw std::runtime_error("malloc() failed in Write()");
        }
        std::memcpy(actualBuffer, buffer, size);
        memoryRegion = ktm_rdma_reg_msgs(rdmaCmId, actualBuffer, size);
        ktm_rdma_post_send(rdmaCmId, nullptr, actualBuffer, size, memoryRegion, IBV_SEND_SIGNALED);
    }

    int num_comp;
    do num_comp = ibv_poll_cq(rdmaCmId->send_cq, 1, &workCompletion); while (num_comp == 0);
    if (num_comp < 0) throw "ibv_poll_cq() failed";
    if (workCompletion.status != IBV_WC_SUCCESS)
        throw std::string("Failed status ") + ibv_wc_status_str(workCompletion.status);

    if (actualBuffer) {
        std::free(actualBuffer);
        actualBuffer = nullptr;
    }

    return size;
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
    if (rdmaCmId) {
        rdma_disconnect(rdmaCmId);
        rdma_destroy_id(rdmaCmId);
        rdmaCmId = nullptr;
    }
}

// Factory function to create an RDMA communicator
extern "C" std::shared_ptr<RdmaCommunicator> create_communicator(std::shared_ptr<gvirtus::communicators::Endpoint> end) {
    std::string hostname = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->address();
    std::string port     = std::to_string(std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->port());

    // Determine if this is a RoCE endpoint
    bool isRoce = std::dynamic_pointer_cast<gvirtus::communicators::Endpoint_Rdma>(end)->suite() == "roce-rdma";

    return std::make_shared<RdmaCommunicator>(hostname, port, isRoce);
}
