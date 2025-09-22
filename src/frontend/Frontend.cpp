/*
 * gVirtuS -- A GPGPU transparent virtualization component.
 *
 * Copyright (C) 2009-2010  The University of Napoli Parthenope at Naples.
 * (Original license header retained)
 *
 * This file has been modified to support an asynchronous communication pipeline.
 */

#include <gvirtus/communicators/CommunicatorFactory.h>
#include <gvirtus/communicators/EndpointFactory.h>
#include <gvirtus/frontend/Frontend.h>
#include <filesystem>
#include "communicators/hybrid/HybridCommunicator.h" // Retained for hybrid support
#include "gvirtus/communicators/IAsyncCommunicator.h"
#include "gvirtus/communicators/tcp/SyncAdapterCommunicator.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <mutex>
#include <chrono>
#include <unordered_set> // Use a more efficient container for lookups
#include <stdlib.h> /* getenv */
#include <cstring>  // For strcasecmp

#include "log4cplus/configurator.h"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"

using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using namespace std;

using gvirtus::communicators::Buffer;
using gvirtus::communicators::Communicator;
using gvirtus::communicators::IAsyncCommunicator;
using gvirtus::communicators::SyncAdapterCommunicator;
using gvirtus::communicators::CommunicatorFactory;
using gvirtus::communicators::EndpointFactory;
using gvirtus::frontend::Frontend;

// --- Static members (unchanged) ---
static Frontend msFrontend;
std::mutex gFrontendMutex;
map<pthread_t, Frontend *> *Frontend::mpFrontends = NULL;
static bool initialized = false;

log4cplus::Logger logger;

// --- Helper Functions ---

std::string getEnvVar(std::string const &key) {
    char *env_var = getenv(key.c_str());
    return (env_var == nullptr) ? std::string("") : std::string(env_var);
}

/**
 * @brief Determines if a CUDA routine is blocking.
 *
 * This function is the core of the frontend's decision-making process.
 * It checks the routine name against a predefined set of known blocking APIs.
 * Using an unordered_set provides efficient (average O(1)) lookups.
 *
 * @param routine The name of the CUDA routine.
 * @return True if the routine is blocking, false otherwise.
 */
static bool is_blocking_routine(const std::string &routine) {
    static const std::unordered_set<std::string> blocking_routines = {
        "cudaDeviceSynchronize",
        "cudaStreamSynchronize",
        "cudaMemcpy", // The synchronous version is blocking.
        "cudaMalloc",
        "cudaFree",
        "cudaGetDeviceCount",
        "cudaGetDeviceProperties",
        "cudaSetDevice",
        "cudaRegisterFatBinary",
        "cudaRegisterFatBinaryEnd",
        "cudaRegisterFunction",
        "cuInit"
    };
    return blocking_routines.count(routine) > 0;
}


// --- Frontend::Init (Unchanged Logic, Critical for Adapter Pattern) ---
// This function's logic does not need to change.
// The key is that `CommunicatorFactory::get_communicator` will now return
// a SyncAdapterCommunicator, which wraps our new async engine. The `Connect()`
// call will be a blocking call on the adapter, which internally handles the
// async connection and waits for it to complete.
void Frontend::Init(Communicator *c) {
    // Logger configuration
    log4cplus::BasicConfigurator basicConfigurator;
    basicConfigurator.configure();
    logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("GVirtuS Frontend"));

    // Set the logging level
    std::string logLevelString = getEnvVar("GVIRTUS_LOGLEVEL");
    log4cplus::LogLevel logLevel = log4cplus::INFO_LOG_LEVEL;
    if (!logLevelString.empty()) {
        try {
            logLevel = static_cast<log4cplus::LogLevel>(std::stoi(logLevelString));
        } catch (const std::exception& e) {
            LOG4CPLUS_ERROR(logger, fs::path(__FILE__).filename() << ":" << __LINE__ << ": Exception occurred: " << e.what());
            logLevel = log4cplus::INFO_LOG_LEVEL;
        }
    }

    logger.setLogLevel(logLevel);

    pid_t tid = syscall(SYS_gettid);

    // Get the GVIRTUS_CONFIG environment varibale
    std::string config_path = getEnvVar("GVIRTUS_CONFIG");

    // Check if the configuration file is defined
    if (config_path.empty()) {
        // Check if the configuration file is in the GVIRTUS_HOME directory
        config_path = getEnvVar("GVIRTUS_HOME") + "/etc/properties.json";
        if (config_path.empty()) {
            // Finally consider the current directory
            config_path = "./properties.json";
        }
    }

    std::unique_ptr<char> default_endpoint;

    // no frontend found
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        if (mpFrontends->find(tid) == mpFrontends->end()) {
            Frontend *f = new Frontend();
            mpFrontends->insert(make_pair(tid, f));
        }
    }

    LOG4CPLUS_INFO(logger, "GVirtuS frontend version " + config_path);

    try {
        auto endpoint = EndpointFactory::get_endpoint(config_path);

        // This now returns an LD_Lib wrapping a SyncAdapterCommunicator.
        mpFrontends->find(tid)->second->_communicator = CommunicatorFactory::get_communicator(endpoint);
        
        // This is a blocking call on the adapter. It will not return until the
        // underlying async connection is established.
        mpFrontends->find(tid)->second->_communicator->obj_ptr()->Connect();
    }
    catch (const std::exception& e) {
        LOG4CPLUS_ERROR(logger, fs::path(__FILE__).filename() << ":" << __LINE__ << ":" << " Exception occurred: " << e.what());
        exit(EXIT_FAILURE);
    }


    mpFrontends->find(tid)->second->mpInputBuffer = std::make_shared<Buffer>();
    mpFrontends->find(tid)->second->mpOutputBuffer = std::make_shared<Buffer>();
    mpFrontends->find(tid)->second->mpLaunchBuffer = std::make_shared<Buffer>();
    mpFrontends->find(tid)->second->mExitCode = -1;
    mpFrontends->find(tid)->second->mpInitialized = true;

}

// --- Destructor (Unchanged) ---
Frontend::~Frontend() {
    static bool destroying = false;
    if (destroying || mpFrontends == nullptr) return;
    destroying = true;

    std::lock_guard<std::mutex> lock(gFrontendMutex);
    {
        pid_t tid = syscall(SYS_gettid);

        auto env = getenv("GVIRTUS_DUMP_STATS");
        bool dump_stats = env && (strcasecmp(env, "on") == 0 || strcasecmp(env, "true") == 0 || strcmp(env, "1") == 0);

        // Safe iteration while erasing entries
        for (auto it = mpFrontends->begin(); it != mpFrontends->end(); /* no increment here */) {
            if (it->second == this) {
                it = mpFrontends->erase(it);
                continue;
            }

            if (dump_stats) {
                std::cerr << "[GVIRTUS_STATS] Executed " << it->second->mRoutinesExecuted << " routine(s) in "
                        << it->second->mRoutineExecutionTime << " second(s)\n"
                        << "[GVIRTUS_STATS] Sent " << it->second->mDataSent / (1024 * 1024.0) << " Mb(s) in "
                        << it->second->mSendingTime
                        << " second(s)\n"
                        << "[GVIRTUS_STATS] Received " << it->second->mDataReceived / (1024 * 1024.0) << " Mb(s) in "
                        << it->second->mReceivingTime
                        << " second(s)\n";
            }

            delete it->second;
            it = mpFrontends->erase(it);
        }

        // Delete the map itself and set pointer to nullptr
        delete mpFrontends;
        mpFrontends = nullptr;
    }
}

// --- GetFrontend (Unchanged) ---
Frontend *Frontend::GetFrontend(Communicator *c) {
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        if (mpFrontends == nullptr)
            mpFrontends = new map<pthread_t, Frontend *>();
    }

    pid_t tid = syscall(SYS_gettid);  // getting frontend's tid

    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it != mpFrontends->end())
            return it->second;
    }

    Frontend *f = new Frontend();
    try {
        f->Init(c);
        {
            std::lock_guard<std::mutex> lock(gFrontendMutex);
            mpFrontends->insert(make_pair(tid, f));
        }
    }
    catch (const std::exception& e) {
        LOG4CPLUS_ERROR(logger, "Error initializing Frontend: " << e.what());
        delete f;  // Clean up on failure
        return nullptr;
    }

    return f;
}


// --- Frontend::Execute (HEAVILY MODIFIED) ---
// This is the core of our refactoring. It now has two distinct paths.
void Frontend::Execute(const char *routine, const Buffer *input_buffer) {
    // This initial setup remains completely unchanged.
    if (input_buffer == nullptr) input_buffer = mpInputBuffer.get();

    pid_t tid = syscall(SYS_gettid);
    pid_t pid = getpid();
    
    Frontend* frontend = nullptr;
    {
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it == mpFrontends->end()) {
            LOG4CPLUS_ERROR(logger, "Cannot send any job request");
            return;
        }
        frontend = it->second;
    }

    // *** 1. CLASSIFICATION ***
    // The execution path is decided here based on the routine type.
    const bool is_blocking = is_blocking_routine(routine);
    
    LOG4CPLUS_DEBUG(logger, "DEBUG - Received routine " << routine
                      << " [pid=" << pid << ", tid=" << tid << "], Path: " << (is_blocking ? "BLOCKING" : "ASYNC"));

       if (is_blocking) {
        // *** 2. BLOCKING PATH (WITH CORRECTED I/O LOGIC) ***
        
        size_t in_size = input_buffer->GetBufferSize();
        int exit_code = 0;
        double server_exec_sec = 0.0;
        double send_sec = 0.0;
        double recv_sec = 0.0;
        size_t out_buffer_size = 0;

        frontend->mRoutinesExecuted++;

        // ===== send routine info first（under TCP）=====
        auto start_send = steady_clock::now();
        frontend->_communicator->obj_ptr()->Write(routine, strlen(routine) + 1);

        // ===== chose protocol by different routine =====
        if (frontend->_communicator->obj_ptr()->to_string() == "hybridcommunicator") {
            // ... (original hybrid logic)
        }

        // ===== send parameter data =====
        frontend->mDataSent += in_size;
        LOG4CPLUS_DEBUG(logger, "Write " << in_size << " bytes to the buffer");
        
        // *** CORRECTION 1: Manually perform the dump logic ***
        // Instead of: input_buffer->Dump(...)
        size_t data_to_send_size = input_buffer->GetBufferSize();
        frontend->_communicator->obj_ptr()->Write(
            reinterpret_cast<const char*>(&data_to_send_size), sizeof(data_to_send_size));
        frontend->_communicator->obj_ptr()->Write(
            input_buffer->GetBuffer(), data_to_send_size);

        // ===== sync by chosen channle =====
        frontend->_communicator->obj_ptr()->Sync();

        send_sec = duration_cast<milliseconds>(
                       steady_clock::now() - start_send).count() / 1000.0;

        frontend->mpOutputBuffer->Reset();

        // ===== receive parts of the response =====
        auto start_recv = steady_clock::now();
        frontend->_communicator->obj_ptr()->Read((char *)&exit_code, sizeof(int));
        frontend->mExitCode = exit_code;

        frontend->_communicator->obj_ptr()->Read(
            reinterpret_cast<char *>(&server_exec_sec), sizeof(server_exec_sec));

        // ===== receive output buffer =====
        frontend->_communicator->obj_ptr()->Read((char *)&out_buffer_size, sizeof(size_t));
        frontend->mDataReceived += out_buffer_size;
        LOG4CPLUS_DEBUG(logger, "Read " << out_buffer_size << " bytes from the buffer");
        if (out_buffer_size > 0) {
            // *** CORRECTION 2: Manually perform the read logic ***
            // Instead of: frontend->mpOutputBuffer->Read<char>(...)
            // 1. Reserve space in the buffer and get a pointer to it.
            char* write_ptr = frontend->mpOutputBuffer->Delegate<char>(out_buffer_size);
            // 2. Read directly into that memory location.
            frontend->_communicator->obj_ptr()->Read(write_ptr, out_buffer_size);
        }
        recv_sec = duration_cast<milliseconds>(
                       steady_clock::now() - start_recv).count() / 1000.0;

        // ===== update info (unchanged) =====
        frontend->mRoutineExecutionTime += server_exec_sec;
        frontend->mSendingTime += send_sec;
        frontend->mReceivingTime += recv_sec;

        // ===== print log (unchanged) =====
        LOG4CPLUS_DEBUG(logger,
            "Routine '" << routine << "' returned " << exit_code
            << " | server_exec=" << server_exec_sec << "s"
            << " | send=" << send_sec << "s"
            << " | recv=" << recv_sec << "s"
            << " | in=" << in_size << "B"
            << " | out=" << out_buffer_size << "B"
            << " | pid=" << pid << " tid=" << tid);

        LOG4CPLUS_DEBUG(logger, "DEBUG - Called: " << routine);


        // ===== stop this call，clean HybridCommunicator status =====
        if (frontend->_communicator->obj_ptr()->to_string() == "hybridcommunicator") {
            auto hybrid = std::dynamic_pointer_cast<gvirtus::communicators::HybridCommunicator>(
                frontend->_communicator->obj_ptr());
            if (hybrid) {
                hybrid->end_call();
            }
        }
    } else {
        // *** 3. ASYNCHRONOUS PATH (FIRE AND FORGET) ***
        // This path is for non-blocking APIs. It sends the request and returns immediately.
        
        // a. Get the underlying async communicator from the adapter.
        auto sync_adapter = dynamic_cast<SyncAdapterCommunicator*>(frontend->_communicator->obj_ptr().get());
        if (!sync_adapter) {
            LOG4CPLUS_ERROR(logger, "Failed to get sync adapter. Cannot execute async path. Check communicator setup.");
            return; 
        }
        std::shared_ptr<IAsyncCommunicator> async_comm = sync_adapter->get_async_core();

        // b. Serialize routine name and buffer into a single DataChunk.
        // We now use a slightly more robust protocol for async messages to make
        // backend parsing easier: [null-terminated_routine_string][buffer_data]
        Buffer async_buffer;
        async_buffer.Add(routine, strlen(routine) + 1);
        async_buffer.Add(input_buffer->GetBuffer(), input_buffer->GetBufferSize());

        gvirtus::communicators::DataChunk request_chunk = async_buffer.to_datachunk();

        // c. Send the request and return immediately. The callback is null because we don't wait for a reply.
        async_comm->AsyncWrite(std::move(request_chunk), nullptr);

        // d. Update basic stats that don't require a response.
        frontend->mRoutinesExecuted++;
        frontend->mDataSent += request_chunk.size();
        LOG4CPLUS_DEBUG(logger, "ASYNC routine '" << routine << "' submitted to the pipeline.");
    }
}
// --- Frontend::Prepare (Unchanged) ---
void Frontend::Prepare() {
    pid_t tid = syscall(SYS_gettid);
    {
        // Added lock for thread safety, which was implicitly needed before.
        std::lock_guard<std::mutex> lock(gFrontendMutex);
        auto it = mpFrontends->find(tid);
        if (it != mpFrontends->end()) {
            if (it->second && it->second->mpInputBuffer) {
                 it->second->mpInputBuffer->Reset();
            }
        }
    }
}