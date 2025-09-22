#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <iostream>

// --- Linux Specific Includes ---
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

// Forward declaration of our handler class to avoid circular dependencies
namespace gvirtus::communicators {
    class AsyncTcpCommunicator;
}

/**
 * @class EventLoop
 * @brief A high-performance I/O event loop for Linux based on epoll.
 *
 * This class is designed specifically for Linux and is not platform-agnostic.
 * It runs a dedicated background thread that uses epoll to monitor multiple
 * file descriptors for I/O events. It also provides a thread-safe mechanism
 * for posting tasks to be executed by the loop's thread.
 */
class EventLoop {
public:
    // Alias for the handler type for clarity.
    using Handler = gvirtus::communicators::AsyncTcpCommunicator;

    /**
     * @brief Constructs the EventLoop, creating epoll and eventfd instances.
     */
    EventLoop() {
        m_epoll_fd = epoll_create1(0);
        if (m_epoll_fd == -1) {
            throw std::runtime_error("Failed to create epoll instance");
        }
        m_wakeup_fd = eventfd(0, EFD_NONBLOCK);
        if (m_wakeup_fd == -1) {
            close(m_epoll_fd);
            throw std::runtime_error("Failed to create eventfd");
        }

        // Register the wakeup fd with epoll, so we can be woken up by other threads.
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET; // Edge-triggered
        event.data.ptr = nullptr; // Special pointer to identify the wakeup event
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_wakeup_fd, &event) == -1) {
            close(m_epoll_fd);
            close(m_wakeup_fd);
            throw std::runtime_error("Failed to add wakeup fd to epoll");
        }
    }

    /**
     * @brief Destructor. Stops the loop and cleans up resources.
     */
    ~EventLoop() {
        Stop();
        close(m_epoll_fd);
        close(m_wakeup_fd);
    }

    // Make the class non-copyable
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /**
     * @brief Starts the event loop in a dedicated background thread.
     */
    void Start() {
        if (!m_is_running.exchange(true)) {
            m_thread = std::thread(&EventLoop::Loop, this);
        }
    }

    /**
     * @brief Stops the event loop and joins its thread.
     */
    void Stop() {
        if (m_is_running.exchange(false)) {
            WakeUp(); // Wake up the loop so it can see the new m_is_running value
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }
    }

    /**
     * @brief Registers a handler with the event loop.
     * Starts monitoring the handler's fd for read events (and errors).
     * @param handler A shared_ptr to the handler to register.
     */
    void Register(std::shared_ptr<Handler> handler);

    /**
     * @brief Unregisters a handler from the event loop.
     * Stops monitoring the handler's file descriptor.
     * @param handler A shared_ptr to the handler to unregister.
     */
    void Unregister(std::shared_ptr<Handler> handler);

    /**
     * @brief Modifies a registered handler's monitoring to include write events.
     * @param handler A shared_ptr to the handler.
     */
    void EnableWriting(std::shared_ptr<Handler> handler);

    /**
     * @brief Modifies a registered handler's monitoring to exclude write events.
     * @param handler A shared_ptr to the handler.
     */
    void DisableWriting(std::shared_ptr<Handler> handler);

    /**
     * @brief Posts a task to be executed by the event loop's thread.
     * This is thread-safe.
     * @param task A function object to be executed.
     */
    void Post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            m_tasks.push_back(std::move(task));
        }
        WakeUp();
    }

private:
    /**
     * @brief The main loop that calls epoll_wait and dispatches events.
     */
    void Loop();

    /**
     * @brief Wakes up the epoll_wait call by writing to the eventfd.
     */
    void WakeUp() {
        uint64_t one = 1;
        ssize_t n = write(m_wakeup_fd, &one, sizeof(one));
        if (n != sizeof(one)) {
            // This is a serious issue, but in a real app, just log it.
            std::cerr << "EventLoop::WakeUp() failed to write to eventfd" << std::endl;
        }
    }

    /**
     * @brief Executes all pending tasks posted by other threads.
     */
    void HandleTasks() {
        std::vector<std::function<void()>> tasks_to_run;
        {
            std::lock_guard<std::mutex> lock(m_task_mutex);
            tasks_to_run.swap(m_tasks);
        }
        for (const auto& task : tasks_to_run) {
            task();
        }
    }

    /**
     * @brief Helper to modify the epoll interest list for a handler.
     */
    void UpdateHandler(std::shared_ptr<Handler> handler, int op, uint32_t events);

    int m_epoll_fd;
    int m_wakeup_fd;
    
    std::thread m_thread;
    std::atomic<bool> m_is_running{false};
    
    std::mutex m_task_mutex;
    std::vector<std::function<void()>> m_tasks;
};