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
 * @brief RdmaCommunicator using:
 *  - magic 100B INLINE (no ibv_query_qp on data path)
 *  - 64MB pre-registered TX/RX memory pools for large payloads (no per-call reg/dereg)
 *  - 5KB preregistered bounce buffer for small non-inline
 */
namespace gvirtus::communicators {
    class RdmaCommunicator : public Communicator {
    private:
        rdma_cm_id * rdmaCmId{nullptr};
        rdma_cm_id * rdmaCmListenId{nullptr};

        char hostname[256]{};
        char port[6]{}; // 5 digits + '\0'
        uint32_t inline_max_ = 0;
        ibv_wc workCompletion{};   // CQ poll scratch

        // Legacy scratch MR pointer (kept for compatibility; not used on pool path)
        ibv_mr * memoryRegion{nullptr};

        // Small bounce buffer (<= 5 KiB non-inline)
        char   preregisteredBuffer[1024 * 5]{};
        ibv_mr * preregisteredMr{nullptr};

        bool isRoce = false;

        // ---- INLINE strategy ----
        static constexpr uint32_t kInlineMagicBytes = 100;        // conservative magic limit
        static constexpr size_t   kSmallThreshold   = 1024 * 5;    // small bounce buffer size
        bool inline_enabled{true}; // disable permanently on first inline post failure

        // ---- 64MB TX/RX pools (pre-registered, ring allocator) ----
        static constexpr size_t kPoolSize = 64ull * 1024ull * 1024ull; // 64 MB
        static constexpr size_t kPoolAlign = 4096; // page alignment

        // TX pool
        char*   tx_pool_{nullptr};
        ibv_mr* tx_mr_{nullptr};
        size_t  tx_head_{0}; // ring head offset in bytes

        // RX pool
        char*   rx_pool_{nullptr};
        ibv_mr* rx_mr_{nullptr};
        size_t  rx_head_{0}; // ring head offset in bytes

        // Helpers
        void request_min_rnr_timer_(uint8_t v);

        // Ring helpers: choose an address in the pool with wrap-around.
        // Returns nullptr if size > kPoolSize (caller should fallback).
        inline char* pool_reserve_tx_(size_t size, size_t &chosen_off);
        inline char* pool_reserve_rx_(size_t size, size_t &chosen_off);

        // Allocate & register pools (called after QP is ready)
        void init_pools_();
        void destroy_pools_();
        void cache_inline_cap_();
        
    public:
        RdmaCommunicator() = default;
        RdmaCommunicator(const std::string& hostname, const std::string& port);
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

        std::string to_string() {return "rdmacommunicator";}
    };
}

#endif //RDMACM_RDMACOMMUNICATOR_H
