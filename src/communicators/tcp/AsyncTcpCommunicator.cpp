#include "AsyncTcpCommunicator.h"
#include <iostream>
#include <stdexcept>
#include <cstring>

// --- Platform specific includes ---
#ifdef __linux__
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include "EventLoop.h" // using a linux event loop implementation
#else
#endif

namespace gvirtus::communicators {

// --- Global Event Loop ---
// A singleton or a global static instance for the event loop is a common pattern.
// This ensures all async operations run on a dedicated I/O thread pool.
static EventLoop& GetEventLoop() {
    static EventLoop loop; // we now in linux evn.
    static bool started = false;
    if (!started) {
        loop.Start();
        started = true;
    }
    return loop;
}

// --- Helper Functions ---
static void make_socket_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) throw std::runtime_error("fcntl(F_GETFL) failed");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl(F_SETFL, O_NONBLOCK) failed");
    }
}


// --- Constructors & Destructor ---

AsyncTcpCommunicator::AsyncTcpCommunicator(std::shared_ptr<Endpoint_Tcp> endpoint)
    : m_endpoint(std::move(endpoint)) {
    std::cout << "AsyncTcpCommunicator created for endpoint." << std::endl;
}

AsyncTcpCommunicator::AsyncTcpCommunicator(int connected_fd, std::shared_ptr<Endpoint_Tcp> endpoint)
    : m_role(Role::CONNECTION), m_endpoint(std::move(endpoint)), m_fd(connected_fd) {
    make_socket_non_blocking(m_fd);
    std::cout << "AsyncTcpCommunicator created for accepted connection fd: " << m_fd << std::endl;
    RegisterWithEventLoop();
}

AsyncTcpCommunicator::~AsyncTcpCommunicator() {
    if (m_fd != -1) {
        // Ensure the socket is closed and unregistered from the event loop
        DoClose();
    }
    std::cout << "AsyncTcpCommunicator destroyed." << std::endl;
}


// --- Public Interface Methods ---

void AsyncTcpCommunicator::Serve(const Endpoint& endpoint, NewConnectionCallback on_new_connection) {
    if (m_role != Role::IDLE) throw std::logic_error("Communicator already in use.");
    
    // We need to cast the base reference to the derived type to get the port.
    const auto* tcp_endpoint = dynamic_cast<const Endpoint_Tcp*>(&endpoint);
    if (!tcp_endpoint) {
        throw std::runtime_error("Invalid endpoint type provided to AsyncTcpCommunicator::Serve");
    }

    m_role = Role::LISTENER;
    m_on_new_connection_cb = std::move(on_new_connection);

    m_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) throw std::runtime_error("Failed to create listening socket.");

    int on = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
    make_socket_non_blocking(m_fd); 

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // *** CRITICAL FIX: Use the 'endpoint' parameter passed to the function. ***
    server_addr.sin_port = htons(tcp_endpoint->port());

    if (bind(m_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        // It's good practice to close the socket on failure.
        close(m_fd); 
        m_fd = -1;
        throw std::runtime_error("Failed to bind socket: " + std::string(strerror(errno)));
    }
    if (listen(m_fd, SOMAXCONN) < 0) {
        close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Failed to listen on socket: " + std::string(strerror(errno)));
    }

    // Use the correct endpoint in the log message.
    std::cout << "Server listening asynchronously on port " << tcp_endpoint->port() << std::endl;
    
    RegisterWithEventLoop();
}
// This is the event handler for incoming connections.
// It's called by the EventLoop thread when the listening socket is readable.

void AsyncTcpCommunicator::Connect(const Endpoint& endpoint, ConnectCallback on_complete) {
    if (m_role != Role::IDLE) throw std::logic_error("Communicator already in use.");
    
    m_role = Role::CONNECTING;
    m_on_connect_cb = std::move(on_complete);
    
    // We need to cast the base reference to the derived type to get address and port.
    const auto* tcp_endpoint = dynamic_cast<const Endpoint_Tcp*>(&endpoint);
    if (!tcp_endpoint) {
        throw std::runtime_error("Invalid endpoint type provided to AsyncTcpCommunicator::Connect");
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result;

    // *** CRITICAL FIX: Use the 'endpoint' parameter passed to the function. ***
    std::string port_str = std::to_string(tcp_endpoint->port());
    int s = getaddrinfo(tcp_endpoint->address().c_str(), port_str.c_str(), &hints, &result);
    
    if (s != 0) {
        throw std::runtime_error("getaddrinfo: " + std::string(gai_strerror(s)));
    }

    // The rest of the function logic remains the same...
    struct addrinfo *rp;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        m_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (m_fd == -1) continue;

        make_socket_non_blocking(m_fd);

        if (::connect(m_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            freeaddrinfo(result);
            m_role = Role::CONNECTION;
            RegisterWithEventLoop();
            if (m_on_connect_cb) m_on_connect_cb(true);
            return;
        }

        if (errno == EINPROGRESS) {
            RegisterWithEventLoop();
            freeaddrinfo(result);
            return;
        }

        close(m_fd);
        m_fd = -1;
    }

    freeaddrinfo(result);
    m_role = Role::IDLE;
    // Use the correct endpoint in the error message
    throw std::runtime_error("Could not initiate connect to " + tcp_endpoint->address());
}


void AsyncTcpCommunicator::AsyncWrite(DataChunk data, WriteCallback on_complete) {
    if (m_role != Role::CONNECTION) return;

    // Protocol: Prepend the size of the data as a 64-bit unsigned integer.
    size_t data_size = data.size();
    DataChunk message_to_send;
    message_to_send.resize(sizeof(data_size));
    memcpy(message_to_send.data(), &data_size, sizeof(data_size));
    message_to_send.insert(message_to_send.end(), data.begin(), data.end());
    
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        m_write_queue.push_back(std::move(message_to_send));
        m_on_write_complete_cb = std::move(on_complete); // Store the latest callback
    }
    // Tell the event loop we want to write.
    GetEventLoop().EnableWriting(shared_from_this());
}

void AsyncTcpCommunicator::SetDataReceivedCallback(DataReceivedCallback on_data_received) {
    if (m_role != Role::CONNECTION) return;
    m_on_data_received_cb = std::move(on_data_received);
}

void AsyncTcpCommunicator::Close() {
    // Post a close request to the event loop's thread to avoid race conditions.
    GetEventLoop().Post([self = shared_from_this()]() {
        self->DoClose();
    });
}

void AsyncTcpCommunicator::DoClose() {
    if (m_fd == -1) return;
    UnregisterFromEventLoop();
    close(m_fd);
    m_fd = -1;
    m_role = Role::IDLE;
    std::cout << "Connection closed." << std::endl;
}


// --- Internal Event Handling ---

void AsyncTcpCommunicator::RegisterWithEventLoop() {
    GetEventLoop().Register(shared_from_this());
}

void AsyncTcpCommunicator::UnregisterFromEventLoop() {
    GetEventLoop().Unregister(shared_from_this());
}
// In AsyncTcpCommunicator.cpp

// New private method to handle the result of a non-blocking connect
// In AsyncTcpCommunicator.cpp

// It's called by the EventLoop thread when the connecting socket is writable (EPOLLOUT).
void AsyncTcpCommunicator::HandleConnect() {
    int optval;
    socklen_t optlen = sizeof(optval);
    
    if (getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0 || optval != 0) {
        // Failure
        if (m_on_connect_cb) m_on_connect_cb(false);
        DoClose();
        return;
    }
    
    // Success
    m_role = Role::CONNECTION;
    // We are now connected. Stop listening for write events until we actually have data to send.
    GetEventLoop().DisableWriting(shared_from_this()); 
    if (m_on_connect_cb) m_on_connect_cb(true);
}
void AsyncTcpCommunicator::HandleEvents(uint32_t events) {
    if (m_role == Role::LISTENER) {
        if (events & EPOLLIN) {
            HandleAccept();
        }
    } else if (m_role == Role::CONNECTION) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            DoClose();
            return;
        }
        if (events & EPOLLIN) {
            HandleRead();
        }
        if (events & EPOLLOUT) {
            HandleWrite();
        }
    }
}

void AsyncTcpCommunicator::HandleAccept() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(m_fd, (sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "accept() error: " << strerror(errno) << std::endl;
            }
            break; // No more incoming connections
        }

        // *** START OF CORRECTION ***
        // Adapt the data to fit the existing Endpoint_Tcp constructor.

        // 1. Convert the raw C-style data into std::string.
        std::string client_ip_str(inet_ntoa(client_addr.sin_addr));
        std::string client_port_str = std::to_string(ntohs(client_addr.sin_port));

        // 2. Call the existing constructor with 4 string arguments.
        //    We can provide sensible defaults for 'suite' and 'protocol'.
        auto client_endpoint = std::make_shared<Endpoint_Tcp>(
            "accepted_connection", // A default suite name
            "tcp",                 // The protocol
            client_ip_str,         // The accepted IP address
            client_port_str        // The accepted port
        );
        // *** END OF CORRECTION ***
        
        try {
            // This part about the private constructor and static factory method
            // is still a valid and necessary fix for the second compilation error.
            auto new_comm = AsyncTcpCommunicator::CreateFromAcceptedSocket(client_fd, client_endpoint);
            
            if (m_on_new_connection_cb) {
                m_on_new_connection_cb(std::move(new_comm));
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to create communicator for new connection: " << e.what() << std::endl;
            close(client_fd);
        }
    }
}
void AsyncTcpCommunicator::HandleRead() {
    char read_buf[65536];
    ssize_t bytes_read;
    while ((bytes_read = read(m_fd, read_buf, sizeof(read_buf))) > 0) {
        m_read_buffer.insert(m_read_buffer.end(), read_buf, read_buf + bytes_read);
    }
    
    if (bytes_read == 0) { // Peer closed connection
        DoClose();
        return;
    }
    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        DoClose();
        return;
    }

    // Protocol Parsing Logic (Length-Prefixed)
    while (true) {
        if (m_next_message_size == 0) {
            if (m_read_buffer.size() >= sizeof(size_t)) {
                memcpy(&m_next_message_size, m_read_buffer.data(), sizeof(size_t));
                m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + sizeof(size_t));
            } else {
                break; // Not enough data for the length prefix
            }
        }

        if (m_next_message_size > 0 && m_read_buffer.size() >= m_next_message_size) {
            DataChunk message(m_read_buffer.begin(), m_read_buffer.begin() + m_next_message_size);
            m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + m_next_message_size);
            m_next_message_size = 0;
            
            if (m_on_data_received_cb) {
                m_on_data_received_cb(std::move(message));
            }
        } else {
            break; // Not enough data for the full message
        }
    }
}

void AsyncTcpCommunicator::HandleWrite() {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    
    while (!m_write_queue.empty()) {
        auto& chunk = m_write_queue.front();
        ssize_t bytes_written = write(m_fd, chunk.data(), chunk.size());

        if (bytes_written > 0) {
            if (bytes_written == chunk.size()) {
                m_write_queue.pop_front();
            } else {
                chunk.erase(chunk.begin(), chunk.begin() + bytes_written);
                break; // Wait for next EPOLLOUT
            }
        } else {
            if (bytes_written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                DoClose();
            }
            break; // Socket buffer is full or error
        }
    }

    // If the write queue is empty, we are no longer interested in write events
    if (m_write_queue.empty()) {
        GetEventLoop().DisableWriting(shared_from_this());
        if (m_on_write_complete_cb) {
            m_on_write_complete_cb(true);
            m_on_write_complete_cb = nullptr; // One-shot callback
        }
    }
}

std::shared_ptr<AsyncTcpCommunicator> AsyncTcpCommunicator::CreateFromAcceptedSocket(int connected_fd, std::shared_ptr<Endpoint_Tcp> endpoint) {
    // Because this static function is PART OF the AsyncTcpCommunicator class,
    // it has the special privilege of being able to call the class's private constructor.
    return std::shared_ptr<AsyncTcpCommunicator>(new AsyncTcpCommunicator(connected_fd, std::move(endpoint)));
}

} // namespace gvirtus::communicators