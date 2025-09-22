#include "gvirtus/backend/Process.h"

// --- New includes for the asynchronous pipeline model ---
#include <gvirtus/communicators/Buffer.h>
#include <gvirtus/communicators/Result.h>
#include "communicators/hybrid/HybridCommunicator.h" // Kept for future hybrid support
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <signal.h>
#include <unistd.h>
#include <iostream>
#include <unordered_set>
#include <chrono>

// Filesystem is needed for plugin loading path construction
#include <filesystem>
namespace fs = std::filesystem;

// Explicitly bring namespaces and classes into scope for clarity
using gvirtus::backend::Process;
using gvirtus::common::LD_Lib;
using gvirtus::communicators::Buffer;
using gvirtus::communicators::IAsyncCommunicator;
using gvirtus::communicators::DataChunk;
using gvirtus::communicators::Result;
using gvirtus::backend::Handler; // Use Handler from the correct namespace

using std::chrono::steady_clock;
using namespace std;

// =========================================================================
// ==                 HELPER FUNCTIONS AND CLASSES                        ==
// =========================================================================

/**
 * @brief Determines if a CUDA routine is blocking. [BACKEND VERSION]
 */
static bool is_blocking_api(const std::string &routine) {
    static const std::unordered_set<std::string> blocking_routines = {
        "cudaDeviceSynchronize", "cudaStreamSynchronize", "cudaMemcpy",
        "cudaMalloc", "cudaFree", "cudaGetDeviceCount",
        "cudaGetDeviceProperties", "cudaSetDevice", "cudaRegisterFatBinary",
        "cudaRegisterFatBinaryEnd", "cudaRegisterFunction", "cuInit"
    };
    return blocking_routines.count(routine) > 0;
}

/**
 * @class ExecutionTask
 * @brief Represents a single unit of work, including classification info.
 */
class ExecutionTask {
public:
    ExecutionTask(DataChunk data, std::shared_ptr<IAsyncCommunicator> client)
        : m_client_comm(std::move(client)) {
        char* ptr = data.data();
        m_routine = std::string(ptr);
        size_t routine_len_with_null = m_routine.length() + 1;

        if (data.size() < routine_len_with_null) {
            throw std::runtime_error("Invalid data chunk: too small for routine name.");
        }
        
        ptr += routine_len_with_null;
        size_t buffer_size = data.size() - routine_len_with_null;
        m_input_buffer = std::make_shared<Buffer>(ptr, buffer_size);

        m_is_blocking = is_blocking_api(m_routine);
    }

    std::string m_routine;
    std::shared_ptr<Buffer> m_input_buffer;
    std::shared_ptr<IAsyncCommunicator> m_client_comm;
    bool m_is_blocking;
};

/**
 * @class TaskQueue
 * @brief A thread-safe queue for ExecutionTask objects.
 */
class TaskQueue {
public:
    void push(std::shared_ptr<ExecutionTask> task) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(task));
        m_cv.notify_one();
    }

    std::shared_ptr<ExecutionTask> pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || m_stop; });
        if (m_stop) return nullptr;
        std::shared_ptr<ExecutionTask> task = std::move(m_queue.front());
        m_queue.pop();
        return task;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
    }
private:
    std::queue<std::shared_ptr<ExecutionTask>> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
};

/**
 * @class WorkerThreadPool
 * @brief Manages a pool of worker threads that consume tasks from a TaskQueue.
 */
class WorkerThreadPool {
public:
    WorkerThreadPool(size_t num_threads, std::shared_ptr<TaskQueue> queue, std::vector<std::shared_ptr<LD_Lib<Handler>>> handlers)
        : m_num_threads(num_threads), m_task_queue(std::move(queue)), m_handlers(std::move(handlers)) {}
    
    ~WorkerThreadPool() {
        Stop();
    }

    void Start() {
        for (size_t i = 0; i < m_num_threads; ++i) {
            m_threads.emplace_back(&WorkerThreadPool::WorkerLoop, this, i);
        }
    }

    void Stop() {
        if (!m_stopped.exchange(true)) {
            m_task_queue->stop();
            for (auto& thread : m_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }
    }

private:
    void WorkerLoop(int thread_id) {
        auto logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("Worker"));
        LOG4CPLUS_DEBUG(logger, "✓ - Worker thread " << thread_id << " started.");
        
        while (true) {
            auto task = m_task_queue->pop();
            if (!task) break; // Queue was stopped

            LOG4CPLUS_DEBUG(logger, "✓ - Worker " << thread_id << " picked up task: '" << task->m_routine << "'" << (task->m_is_blocking ? " (BLOCKING)" : " (ASYNC)"));

            std::shared_ptr<Handler> h = nullptr;
            for (auto &ptr_el : m_handlers) {
                if (ptr_el->obj_ptr()->CanExecute(task->m_routine)) {
                    h = ptr_el->obj_ptr();
                    break;
                }
            }

            std::shared_ptr<Result> result;
            if (h == nullptr) {
                LOG4CPLUS_ERROR(logger, "✖ - Requested unknown routine '" << task->m_routine << "'");
                result = std::make_shared<Result>(-1, std::make_shared<Buffer>());
            } else {
                auto start = steady_clock::now();
                result = h->Execute(task->m_routine, task->m_input_buffer);
                result->TimeTaken(std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now() - start).count() / 1000.0);
            }

            result->DumpAsync(task->m_client_comm);
        }
        LOG4CPLUS_DEBUG(logger, "✓ - Worker thread " << thread_id << " stopped.");
    }

    size_t m_num_threads;
    std::shared_ptr<TaskQueue> m_task_queue;
    std::vector<std::thread> m_threads;
    std::vector<std::shared_ptr<LD_Lib<Handler>>> m_handlers;
    std::atomic<bool> m_stopped{false};
};

// =========================================================================
// ==                      PROCESS CLASS IMPLEMENTATION                   ==
// =========================================================================

Process::Process(std::shared_ptr<IAsyncCommunicator> communicator, vector<string> &plugins) : Observable() {
    logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("Process"));

    log4cplus::LogLevel logLevel = log4cplus::INFO_LOG_LEVEL;
    char *val = getenv("GVIRTUS_LOGLEVEL");
    std::string logLevelString = (val == NULL ? "" : std::string(val));
    if (!logLevelString.empty()) {
        try {
            logLevel = std::stoi(logLevelString);
        } catch (...) { /* ignore parsing errors */ }
    }
    logger.setLogLevel(logLevel);

    signal(SIGCHLD, SIG_IGN);
    m_listener = std::move(communicator);
    m_endpoint = std::move(endpoint);
    
    mPlugins = plugins;
}

std::string getGVirtuSHome() {
    char *val = getenv("GVIRTUS_HOME");
    return val == NULL ? "" : std::string(val);
}

void Process::Start() {
    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] Process::Start() called for asynchronous model.");

    // Load Handlers (Plugins)
    for_each(mPlugins.begin(), mPlugins.end(), [&](const std::string &plug) {
        std::string gvirtus_home = getGVirtuSHome();
        std::string to_append = "libgvirtus-plugin-" + plug + ".so";
        auto ld_path = fs::path(gvirtus_home + "/lib").append(to_append);
        try {
            auto dl = std::make_shared<LD_Lib<Handler>>(ld_path, "create_t");
            dl->build_obj();
            _handlers.push_back(dl);
        } catch (const std::string &e) {
            LOG4CPLUS_ERROR(logger, e);
        }
    });

    // Initialize the pipeline infrastructure
    m_task_queue = std::make_shared<TaskQueue>();
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // Fallback for safety
    m_worker_pool = std::make_shared<WorkerThreadPool>(num_threads, m_task_queue, _handlers);

    // Start the worker threads
    m_worker_pool->Start();

    // Define the callback for handling new connections
    auto on_new_connection = [this](std::shared_ptr<IAsyncCommunicator> new_conn) {
        LOG4CPLUS_INFO(logger, "✓ - [I/O Thread in PID " << getpid() << "] New connection accepted.");
        
        // Wire the new connection to the pipeline
        new_conn->SetDataReceivedCallback([this, new_conn](DataChunk data) {
            try {
                auto task = std::make_shared<ExecutionTask>(std::move(data), new_conn);
                this->m_task_queue->push(task);
            } catch (const std::exception& e) {
                LOG4CPLUS_ERROR(logger, "✖ - Error parsing incoming data: " << e.what() << ". Closing connection.");
                new_conn->Close();
            }
        });
    };

    try {
        // Start the asynchronous server. This call is non-blocking.
        m_listener->Serve(*m_endpoint, on_new_connection);
        LOG4CPLUS_INFO(logger, "✓ - [Process " << getpid() << "] Async service running. Waiting for signals.");

        // The main thread of this process now waits for termination signals.
        pause(); 
        LOG4CPLUS_INFO(logger, "✓ - [Process " << getpid() << "] Signal received, initiating shutdown.");

    } catch (const std::exception &exc) {
        LOG4CPLUS_ERROR(logger, "✖ - [Process " << getpid() << "]: A critical error occurred during startup: " << exc.what());
    }

    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] Shutting down pipeline...");
    m_worker_pool->Stop();
    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] Process::Start() returned.");
}

Process::~Process() {
    // Smart pointers will handle deallocation of listener, queue, and pool.
    // The worker pool's destructor calls Stop() to ensure threads are joined.
    _handlers.clear();
    mPlugins.clear();
    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] Process object destroyed.");
}