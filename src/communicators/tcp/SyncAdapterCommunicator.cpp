#include "SyncAdapterCommunicator.h"
#include <stdexcept>
#include <vector>
#include <condition_variable>
#include <cstring> // For memcpy

namespace gvirtus::communicators {

// --- MODIFIED CONSTRUCTOR ---
// It now accepts and stores the endpoint.
SyncAdapterCommunicator::SyncAdapterCommunicator(std::shared_ptr<IAsyncCommunicator> async_core, std::shared_ptr<Endpoint> endpoint)
    : m_async_core(std::move(async_core)), m_endpoint(std::move(endpoint)) {
    if (!m_async_core) {
        throw std::invalid_argument("Async core cannot be null.");
    }

    // Set up the callback to populate our internal read queue.
    m_async_core->SetDataReceivedCallback([this](DataChunk data) {
        std::lock_guard<std::mutex> lock(m_read_mutex);
        m_read_queue.push(std::move(data));
        m_read_cv.notify_one();
    });
}

// --- Synchronous Interface Implementation ---

void SyncAdapterCommunicator::Connect() {
    if (!m_endpoint) {
        throw std::logic_error("Endpoint is not set for Connect operation.");
    }
    
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();

    // *** CORRECTION: Pass the required Endpoint parameter. ***
    m_async_core->Connect(*m_endpoint, [&promise](bool success) {
        promise.set_value(success);
    });

    if (!future.get()) {
        throw std::runtime_error("SyncAdapter: Failed to connect.");
    }
}

size_t SyncAdapterCommunicator::Read(char *buffer, size_t size) {
    if (size == 0) return 0;

    std::unique_lock<std::mutex> lock(m_read_mutex);
    m_read_cv.wait(lock, [this]{ return !m_read_queue.empty() || !m_read_buffer.empty(); });

    size_t bytes_copied = 0;
    while (bytes_copied < size) {
        if (m_read_buffer.empty()) {
            if (m_read_queue.empty()) { // Should not happen due to cv.wait, but for safety
                 m_read_cv.wait(lock, [this]{ return !m_read_queue.empty(); });
            }
            m_read_buffer = std::move(m_read_queue.front());
            m_read_queue.pop();
        }

        size_t to_copy = std::min(size - bytes_copied, m_read_buffer.size());
        memcpy(buffer + bytes_copied, m_read_buffer.data(), to_copy);
        bytes_copied += to_copy;
        m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + to_copy);

        if (bytes_copied < size && m_read_queue.empty()) {
            m_read_cv.wait(lock, [this]{ return !m_read_queue.empty(); });
        }
    }
    return bytes_copied;
}

size_t SyncAdapterCommunicator::Write(const char *buffer, size_t size) {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    m_write_buffer.insert(m_write_buffer.end(), buffer, buffer + size);
    return size;
}

void SyncAdapterCommunicator::Sync() {
    DataChunk data_to_send;
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        if (m_write_buffer.empty()) return;
        data_to_send.assign(m_write_buffer.begin(), m_write_buffer.end());
        m_write_buffer.clear();
    }

    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();

    m_async_core->AsyncWrite(std::move(data_to_send), [&promise](bool success) {
        promise.set_value(success);
    });

    if (!future.get()) {
        throw std::runtime_error("SyncAdapter: Sync (AsyncWrite) failed.");
    }
}

void SyncAdapterCommunicator::Close() {
    m_async_core->Close();
}

void SyncAdapterCommunicator::Serve() {
    if (!m_endpoint) {
        throw std::logic_error("Endpoint is not set for Serve operation.");
    }
    
    // *** CORRECTION: Pass the required Endpoint parameter. ***
    m_async_core->Serve(*m_endpoint, [this](std::shared_ptr<IAsyncCommunicator> new_connection) {
        std::lock_guard<std::mutex> lock(m_accept_mutex);
        m_accept_queue.push(new_connection);
        m_accept_cv.notify_one();
    });
}

const Communicator *const SyncAdapterCommunicator::Accept() const {
    std::unique_lock<std::mutex> lock(m_accept_mutex);
    m_accept_cv.wait(lock, [this]{ return !m_accept_queue.empty(); });

    std::shared_ptr<IAsyncCommunicator> async_new_conn = m_accept_queue.front();
    m_accept_queue.pop();
    
    // *** CORRECTION: Call the constructor that matches the .h file. ***
    // Pass nullptr for the endpoint, as this accepted communicator won't initiate connections.
    return new SyncAdapterCommunicator(async_new_conn, nullptr);
}

} // namespace gvirtus::communicators