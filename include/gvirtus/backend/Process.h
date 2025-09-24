#pragma once

#include <gvirtus/common/LD_Lib.h>
#include <gvirtus/common/Observable.h>
#include <gvirtus/communicators/IAsyncCommunicator.h> // MODIFIED: Include the new async interface
#include <gvirtus/communicators/Endpoint.h>
#include "Handler.h"
#include <memory>
#include <string>
#include <vector>
#include "log4cplus/logger.h"

// Forward-declare the helper classes to avoid including their full definitions here.
// This keeps the header file clean and reduces compile times.
class TaskQueue;
class WorkerThreadPool;

namespace gvirtus::backend {

/**
 * @class Process
 * @brief Manages a self-contained, high-performance asynchronous pipeline for a
 * specific endpoint.
 *
 * In the refactored design, a Process object no longer handles requests directly.
 * Instead, it owns and orchestrates an I/O event loop (via its listener),
 * a task queue, and a pool of worker threads to process requests concurrently.
 */
class Process : public common::Observable {
 public:
  /**
   * @brief Constructs the Process pipeline manager.
   * @param communicator An IAsyncCommunicator instance configured for listening.
   * @param plugins A vector of plugin names to be loaded by the worker threads.
   */
  Process(
      std::shared_ptr<communicators::IAsyncCommunicator> communicator,
      std::shared_ptr<communicators::Endpoint> endpoint,
      std::vector<std::string> &plugins
  );

  /**
   * @brief Destructor. Ensures the pipeline is gracefully shut down.
   */
  ~Process() override;

  /**
   * @brief Starts the asynchronous pipeline.
   * This method initializes and starts the worker thread pool, begins listening
   * for connections asynchronously, and then blocks the calling thread (main
   * thread of the child process) until a shutdown signal is received.
   */
  void Start();

 private:
  // --- OLD MEMBERS (some removed, some repurposed) ---
  // The old synchronous communicator is no longer needed.
  // std::shared_ptr<common::LD_Lib<communicators::Communicator, ...>> _communicator; 
  
  // _handlers will be loaded in Start() and passed to the WorkerThreadPool.
  std::vector<std::shared_ptr<common::LD_Lib<Handler>>> _handlers;
 
  std::vector<std::string> mPlugins;
  log4cplus::Logger logger;

  // --- NEW MEMBERS FOR THE ASYNCHRONOUS PIPELINE ---
  
  /**
   * @brief The asynchronous listener that accepts new connections.
   */
  std::shared_ptr<communicators::IAsyncCommunicator> m_listener;
  
  /**
   * @brief The central thread-safe queue for incoming tasks.
   */
  std::shared_ptr<TaskQueue> m_task_queue;
  std::shared_ptr<communicators::Endpoint> m_endpoint; // <-- The missing member
  /**
   * @brief The pool of worker threads that execute tasks from the queue.
   */
  std::shared_ptr<WorkerThreadPool> m_worker_pool;
};

}  // namespace gvirtus::backend