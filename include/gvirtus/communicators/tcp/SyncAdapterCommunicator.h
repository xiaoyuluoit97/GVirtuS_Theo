#pragma once

#include <future>
#include <memory>

#include "gvirtus/communicators/Communicator.h"       // It MUST inherit from the OLD interface
#include "gvirtus/communicators/IAsyncCommunicator.h" // It MUST hold a pointer to the NEW interface

namespace gvirtus::communicators {

/**
 * @class SyncAdapterCommunicator
 * @brief Acts as a bridge between synchronous callers and an asynchronous backend.
 *
 * This class implements the synchronous Communicator interface that old code
 * (like Frontend) expects. Internally, it holds a pointer to a true
 * IAsyncCommunicator and translates the blocking calls (Read, Write, Connect)
 * into non-blocking async calls, using std::promise and std::future to
 * block the calling thread and wait for the result.
 */
class SyncAdapterCommunicator : public Communicator {
public:
    // The constructor takes the real async engine it needs to wrap.
    explicit SyncAdapterCommunicator(std::shared_ptr<IAsyncCommunicator> async_core);

    virtual ~SyncAdapterCommunicator() = default;

    // --- Implementation of the OLD Synchronous Interface ---
    // These methods will be implemented using the promise/future pattern.
    void Serve() override;
    const Communicator *const Accept() const override;
    void Connect() override;
    size_t Read(char *buffer, size_t size) override;
    size_t Write(const char *buffer, size_t size) override;
    void Sync() override;
    void Close() override;
    std::string to_string() override { return "sync_adapter_over_async"; }

    // --- Special method for the new async path ---
    /**
     * @brief Provides access to the underlying asynchronous core.
     *
     * This is the "backdoor" used by the non-blocking path in Frontend::Execute
     * to get the real async communicator and perform fire-and-forget writes.
     * @return A shared_ptr to the underlying IAsyncCommunicator.
     */
    std::shared_ptr<IAsyncCommunicator> get_async_core() {
        return m_async_core;
    }

private:
    std::shared_ptr<IAsyncCommunicator> m_async_core;
};

} // namespace gvirtus::communicators