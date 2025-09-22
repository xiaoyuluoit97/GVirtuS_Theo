#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>

#include "gvirtus/communicators/Communicator.h"
#include "gvirtus/communicators/IAsyncCommunicator.h"
#include "gvirtus/communicators/Endpoint_Tcp.h" // Needed for Serve/Connect

namespace gvirtus::communicators {

/**
 * @class SyncAdapterCommunicator
 * @brief Acts as a bridge between synchronous callers and an asynchronous backend.
 */
class SyncAdapterCommunicator : public Communicator {
public:
    explicit SyncAdapterCommunicator(std::shared_ptr<IAsyncCommunicator> async_core, std::shared_ptr<Endpoint> endpoint);
    virtual ~SyncAdapterCommunicator() = default;

    // --- Implementation of the OLD Synchronous Interface ---
    void Serve() override;
    const Communicator *const Accept() const override;
    void Connect() override;
    size_t Read(char *buffer, size_t size) override;
    size_t Write(const char *buffer, size_t size) override;
    void Sync() override;
    void Close() override;
    std::string to_string() override { return "sync_adapter_over_async"; }

    // --- Special method for the new async path ---
    std::shared_ptr<IAsyncCommunicator> get_async_core() {
        return m_async_core;
    }

private:
    std::shared_ptr<IAsyncCommunicator> m_async_core;
    std::shared_ptr<Endpoint> m_endpoint; // Store endpoint for connect/serve

    // --- Members for Read operation ---
    mutable std::mutex m_read_mutex;
    mutable std::condition_variable m_read_cv;
    mutable std::queue<DataChunk> m_read_queue;
    mutable DataChunk m_read_buffer; // Buffer for partial reads

    // --- Members for Write operation ---
    mutable std::mutex m_write_mutex;
    mutable std::deque<char> m_write_buffer; // Use deque for efficient front removal

    // --- Members for Accept operation ---
    mutable std::mutex m_accept_mutex;
    mutable std::condition_variable m_accept_cv;
    mutable std::queue<std::shared_ptr<IAsyncCommunicator>> m_accept_queue;
};

} // namespace gvirtus::communicators