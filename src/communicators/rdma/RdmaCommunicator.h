//
// Created by Mariano Aponte on 07/12/23.
//

#ifndef RDMACM_RDMACOMMUNICATOR_H
#define RDMACM_RDMACOMMUNICATOR_H

#include "gvirtus/communicators/Communicator.h"
#include "ktmrdma.h"

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <infiniband/verbs.h>

#include <string>
#include <netdb.h>
#include <cstdint>

#define BACKLOG 8
#define BUF_SIZE 1024
#define DEBUG

/**
 * @brief RdmaCommunicator represents a communication interface using RDMA (Remote Direct Memory Access).
 *        - Caches QP inline capability once (no ibv_query_qp on the hot path).
 *        - Uses inline send for small messages when supported by the device.
 */
namespace gvirtus::communicators {
    class RdmaCommunicator : public Communicator {
    private:
        rdma_cm_id * rdmaCmId{nullptr};
        rdma_cm_id * rdmaCmListenId{nullptr};

        char hostname[256]{};
        char port[6]{}; // 5 digits + '\0'

        ibv_wc workCompletion{};   // Scratch WC for CQ poll

        // Optional MR used for large one-shot ops (registered per call)
        ibv_mr * memoryRegion{nullptr};

        // Pre-registered bounce buffer for small non-inline messages (<= 5 KiB)
        char   preregisteredBuffer[1024 * 5]{};
        ibv_mr * preregisteredMr{nullptr};

        // Whether we are using RoCE TCP port space (vs IB)
        bool isRoce = false;

        // Cached inline capability for this QP (bytes). 0 means "inline not supported".
        uint32_t max_inline_data{0};

        // Internal helpers
        void cache_max_inline_();                 // Query QP once and cache inline limit
        void request_min_rnr_timer_(uint8_t v);   // Best-effort tweak of min_rnr_timer

        // Constant: small-message bounce buffer threshold (matches preregisteredBuffer)
        static constexpr size_t kSmallThreshold = 1024 * 5;

    public:
        RdmaCommunicator() = default;
        RdmaCommunicator(const std::string& hostname, const std::string& port)
            : RdmaCommunicator(hostname, port, /*isRoce=*/false) {}
        RdmaCommunicator(const std::string& hostname, const std::string& port, bool isRoce);
        explicit RdmaCommunicator(rdma_cm_id * rdmaCmId);

        ~RdmaCommunicator() override;

        // Passive (server) side
        void Serve();
        const Communicator *const Accept() const;

        // Active (client) side
        void Connect();

        // Data plane
        size_t Read(char * buffer, size_t size) override;
        size_t Write(const char * buffer, size_t size) override;

        void Sync() override;

        void Close() override;

        std::string to_string() { return "rdmacommunicator"; }
    };
}

#endif //RDMACM_RDMACOMMUNICATOR_H
