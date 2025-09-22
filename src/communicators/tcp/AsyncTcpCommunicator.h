#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "gvirtus/communicators/Endpoint.h"
#include "gvirtus/communicators/IAsyncCommunicator.h"

// Now, we can include the derived class header.
#include "gvirtus/communicators/Endpoint_Tcp.h"

// Forward declaration for the event loop engine
class EventLoop;

namespace gvirtus::communicators {

/**
 * @class AsyncTcpCommunicator
 * @brief An asynchronous TCP/IP communicator that can act as a Listener or a Connection.
 *
 * This class uses a shared, static event loop.
 * When used as a server, one instance acts as a listener. For each accepted
 * connection, it creates a new instance to handle that specific connection.
 */
class AsyncTcpCommunicator : public IAsyncCommunicator,
                             public std::enable_shared_from_this<AsyncTcpCommunicator> {
public:
    // Constructor for creating a listener or an outgoing connection
    explicit AsyncTcpCommunicator(std::shared_ptr<Endpoint_Tcp> endpoint);
    static std::shared_ptr<AsyncTcpCommunicator> CreateFromAcceptedSocket(int connected_fd, std::shared_ptr<Endpoint_Tcp> endpoint);
    
    // Destructor
    virtual ~AsyncTcpCommunicator();
    int GetFd() const { return m_fd; }                            
    // Make the class non-copyable
    AsyncTcpCommunicator(const AsyncTcpCommunicator&) = delete;
    AsyncTcpCommunicator& operator=(const AsyncTcpCommunicator&) = delete;

    // --- IAsyncCommunicator Interface Implementation ---
    void Serve(const Endpoint& endpoint, NewConnectionCallback on_new_connection) override;
    void Connect(const Endpoint& endpoint, ConnectCallback on_complete) override;
    void AsyncWrite(DataChunk data, WriteCallback on_complete) override;
    void SetDataReceivedCallback(DataReceivedCallback on_data_received) override;
    void Close() override;

private:
    // Private constructor for creating a connection from an accepted socket
    AsyncTcpCommunicator(int connected_fd, std::shared_ptr<Endpoint_Tcp> endpoint);

    // Internal helper methods
    void RegisterWithEventLoop();
    void UnregisterFromEventLoop();
    void HandleEvents(uint32_t events); // Called by the EventLoop
    void HandleAccept();
    void HandleRead();
    void HandleWrite();
    void HandleConnect();
    void DoClose();

    friend class ::EventLoop; // Allow EventLoop to call HandleEvents

    enum class Role { IDLE, LISTENER, CONNECTION, CONNECTING };
    Role m_role{Role::IDLE};

    std::shared_ptr<Endpoint_Tcp> m_endpoint;
    int m_fd{-1}; // Can be listener_fd or connection_fd

    // State for Connection role
    std::mutex m_write_mutex;
    std::deque<DataChunk> m_write_queue;
    DataChunk m_read_buffer;
    size_t m_next_message_size{0};

    // Callbacks
    NewConnectionCallback m_on_new_connection_cb;
    ConnectCallback m_on_connect_cb;
    DataReceivedCallback m_on_data_received_cb;
    // We'll track a single callback for all writes for simplicity
    WriteCallback m_on_write_complete_cb; 
};

} // namespace gvirtus::communicators