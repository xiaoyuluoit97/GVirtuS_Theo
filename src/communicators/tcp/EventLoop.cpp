#include "EventLoop.h"
#include "AsyncTcpCommunicator.h" // Include the full definition for implementation

// Need to define this function, which is declared in AsyncTcpCommunicator.h
// This is a bit of a workaround for the friend declaration. A better design
// would be to have an IEventHandler interface.
int get_fd_from_handler(const std::shared_ptr<EventLoop::Handler>& handler);


void EventLoop::UpdateHandler(std::shared_ptr<Handler> handler, int op, uint32_t events) {
    epoll_event event{};
    event.events = events;
    event.data.ptr = handler.get();

    // *** MODIFICATION HERE ***
    // Instead of calling a helper function, we directly call the public method.
    int fd = handler->GetFd(); 

    if (epoll_ctl(m_epoll_fd, op, fd, &event) == -1) {
        std::cerr << "epoll_ctl op=" << op << " failed for fd=" << fd << ": " << strerror(errno) << std::endl;
    }
}

void EventLoop::Unregister(std::shared_ptr<Handler> handler) {
    // *** MODIFICATION HERE ***
    int fd = handler->GetFd();

    if (fd != -1) {
        // We don't need to call UpdateHandler here, a direct epoll_ctl is fine.
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1) {
             std::cerr << "epoll_ctl op=DEL failed for fd=" << fd << ": " << strerror(errno) << std::endl;
        }
    }
}

void EventLoop::EnableWriting(std::shared_ptr<Handler> handler) {
    UpdateHandler(handler, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP);
}

void EventLoop::DisableWriting(std::shared_ptr<Handler> handler) {
    UpdateHandler(handler, EPOLL_CTL_MOD, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP);
}


void EventLoop::Loop() {
    std::vector<epoll_event> events(64);

    while (m_is_running) {
        int num_events = epoll_wait(m_epoll_fd, events.data(), events.size(), -1);

        if (num_events < 0) {
            if (errno == EINTR) continue; // Interrupted by a signal, just restart the wait
            std::cerr << "EventLoop::Loop() epoll_wait error: " << strerror(errno) << std::endl;
            continue;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].data.ptr == nullptr) { // This is our wakeup event
                uint64_t count;
                read(m_wakeup_fd, &count, sizeof(count)); // Drain the eventfd
            } else {
                // Here, we cast the raw pointer back to the object type we know it is.
                // This is safe because we control what we put into epoll.
                auto* handler = static_cast<Handler*>(events[i].data.ptr);
                
                // CRITICAL: We need a shared_ptr to ensure the handler is alive
                // during the callback. This is often solved by having a map
                // from fd to shared_ptr within the EventLoop. For simplicity, we assume
                // the handler's lifetime is managed correctly elsewhere.
                try {
                    handler->HandleEvents(events[i].events);
                } catch (const std::exception& e) {
                    std::cerr << "Exception in HandleEvents: " << e.what() << std::endl;
                }
            }
        }
        
        // After handling I/O events, handle any tasks posted from other threads.
        HandleTasks();
    }
}