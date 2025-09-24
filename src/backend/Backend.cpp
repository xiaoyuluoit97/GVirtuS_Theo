#include "gvirtus/backend/Backend.h"

// Include all necessary headers
#include "gvirtus/backend/Process.h"
#include <gvirtus/communicators/CommunicatorFactory.h>
#include <gvirtus/communicators/EndpointFactory.h>
#include <gvirtus/common/JSON.h>

#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <csignal> // For signal()

using gvirtus::backend::Backend;
using gvirtus::backend::Process;
using gvirtus::backend::Property;

// --- CONSTRUCTOR: ONLY parses config, DOES NOT create any complex objects ---
Backend::Backend(const fs::path &path) {
    // Logger setup for the parent process
    this->logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("Backend"));
    char *logLevel_envVar = getenv("GVIRTUS_LOGLEVEL");
    std::string logLevelString = (logLevel_envVar == nullptr ? std::string("") : std::string(logLevel_envVar));
    log4cplus::LogLevel logLevel = logLevelString.empty() ? log4cplus::INFO_LOG_LEVEL : std::stoi(logLevelString);
    this->logger.setLogLevel(logLevel);

    // Resolve and store the configuration path
    try {
        m_path = fs::canonical(path);
    } catch (const fs::filesystem_error& e) {
        LOG4CPLUS_FATAL(logger, "✖ - Configuration file path error: " << e.what());
        exit(EXIT_FAILURE);
    }
    LOG4CPLUS_DEBUG(logger, "✓ - Using canonical configuration path: " << m_path.string());

    // Parse properties just to get the number of endpoints for the fork loop.
    // NO Process objects are created here.
    try {
        _properties = common::JSON<Property>(m_path).parser();
    } catch (const std::exception& e) {
        LOG4CPLUS_FATAL(logger, "✖ - Failed to parse JSON configuration: " << e.what());
        exit(EXIT_FAILURE);
    }

    LOG4CPLUS_INFO(logger, "🛈  - Backend Initialization complete. Processes will be forked in Start().");
}

// --- DESTRUCTOR ---
Backend::~Backend() {
    // Empty body is sufficient.
}

// --- START METHOD: Forks children, and ONLY children create resources ---
void Backend::Start() {
    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] " << "Backend::Start() called.");

    int pid = 0;

    for (int i = 0; i < _properties.endpoints(); i++) {
        activeChilds++;
        if ((pid = fork()) == 0) {
            // ===========================================
            // ===== CHILD PROCESS EXECUTION BLOCK =====
            // ===========================================
            // This is the ONLY place where Process, Communicator, and EventLoop are created.
            
            try {
                // 1. Re-parse the JSON file to get a clean Property object.
                Property child_properties = common::JSON<Property>(m_path).parser();
                
                // 2. Get the Endpoint.
                auto endpoint = communicators::EndpointFactory::get_endpoint(m_path);
                
                // 3. Get the async Communicator. This is the FIRST time GetEventLoop() is called
                //    in the child, so it creates a new, clean EventLoop.
                auto communicator = communicators::CommunicatorFactory::get_async_communicator(
                                        endpoint, 
                                        child_properties.secure()
                                    )->obj_ptr();

                // 4. Create the Process object.
                Process child_process(
                    communicator,
                    endpoint,
                    child_properties.plugins().at(i)
                );

                // 5. Start the pipeline.
                child_process.Start();
            
            } catch (const std::exception& e) {
                // Use a fresh logger instance in the child for safety.
                log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("ChildProcess")).log(log4cplus::FATAL_LOG_LEVEL, 
                    "✖ - Child process failed to initialize and start: " + std::string(e.what()));
                exit(EXIT_FAILURE);
            }
            
            LOG4CPLUS_TRACE(logger, "Child process shutting down gracefully.");
            exit(EXIT_SUCCESS);
        }
    }

    /* PARENT PROCESS LOGIC */
    if (pid != 0) {
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        LOG4CPLUS_TRACE(logger, "Active childs: " << activeChilds);

        int status;
        while (activeChilds > 0) {
            LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] " << "Waiting for childs. " << activeChilds << " remaining.");
            int waitres = wait(&status);
            
            if (waitres > 0) {
                if (WIFEXITED(status)) {
                    LOG4CPLUS_TRACE(logger, "Process " << waitres << " exited with status " << WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    LOG4CPLUS_ERROR(logger, "Process " << waitres << " was terminated by signal " << WTERMSIG(status));
                }
                activeChilds--;
            } else {
                 if (errno == ECHILD) {
                    LOG4CPLUS_TRACE(logger, "No child processes left to wait for.");
                    break;
                 }
                 if (errno == EINTR) continue;
                 LOG4CPLUS_TRACE(logger, "Error on wait(): " << strerror(errno));
                 break;
            }
        }

        LOG4CPLUS_INFO(logger, "✓ - All child processes have terminated. Backend will now exit.");
        // A simple exit might be better than pause if all children are meant to be managed.
        // For now, keeping pause() to match original intent.
        // signal(SIGINT, sigint_handler);
        pause();
    }

    LOG4CPLUS_DEBUG(logger, "✓ - [Process " << getpid() << "] " << "Backend::Start() returned.");
}

void Backend::EventOccurred(std::string &event, void *object) {
    LOG4CPLUS_DEBUG(logger, "✓ - EventOccurred: " << event);
}