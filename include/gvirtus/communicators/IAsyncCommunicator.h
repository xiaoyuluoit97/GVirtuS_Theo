// File: include/gvirtus/communicators/IAsyncCommunicator.h

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward-declare everything to break cycles.
namespace gvirtus::communicators {
    class Endpoint;
    class IAsyncCommunicator;
}

namespace gvirtus::communicators {

    using DataChunk = std::vector<char>;
    using ConnectCallback = std::function<void(bool success)>;
    using WriteCallback = std::function<void(bool success)>;
    // Let's use shared_ptr here, as it's the most robust for async callbacks and works with forward declarations.
    using NewConnectionCallback = std::function<void(std::shared_ptr<IAsyncCommunicator> new_connection)>;
    using DataReceivedCallback = std::function<void(DataChunk data)>;

    class IAsyncCommunicator {
    public:
        virtual ~IAsyncCommunicator() = default;

        // Keep the const reference, as per your requirement.
        virtual void Serve(const Endpoint& endpoint, NewConnectionCallback on_new_connection) = 0;
        virtual void Connect(const Endpoint& endpoint, ConnectCallback on_complete) = 0;
        
        virtual void AsyncWrite(DataChunk data, WriteCallback on_complete) = 0;
        virtual void SetDataReceivedCallback(DataReceivedCallback on_data_received) = 0;
        virtual void Close() = 0;
    };

} // namespace gvirtus::communicators