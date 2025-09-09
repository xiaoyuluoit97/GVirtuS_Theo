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
 *        - Uses a magic 100B threshold for INLINE sends (no ibv_query_qp on any path).
 *        - If an INLINE post fails once, INLINE is permanently disabled and the code falls back automatically.
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

        bool isRoce = false;

        // --- INLINE strategy (magic threshold, no queries) ---
        static constexpr uint32_t kInlineMagicBytes = 100;   // conservative magic size
        static constexpr size_t   kSmallThreshold   = 1024 * 5; // matches preregisteredBuffer
        bool inline_enabled{true}; // after first inline failure, set to false

        // Helper: best-effort tweak of min_rnr_timer
        void request_min_rnr_timer_(uint8_t v);

    public:
        RdmaCommunicator() = default;
        RdmaCommunicator(const std::string& hostname, const std::string& port);
        RdmaCommunicator(const std::string& hostname, const std::string& port, bool isRoce);
        explicit RdmaCommunicator(rdma_cm_id * rdmaCmId);

        ~RdmaCommunicator() override;

        void Serve();
        const Communicator *const Accept() const;

        void Connect();

        size_t Read(char * buffer, size_t size) override;
        size_t Write(const char * buffer, size_t size) override;

        void Sync() override;

        void Close() override;

        std::string to_string() {return "rdmacommunicator";}
    };
}

#endif //RDMACM_RDMACOMMUNICATOR_H
