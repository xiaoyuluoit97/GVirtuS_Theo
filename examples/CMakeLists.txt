cmake_minimum_required(VERSION 3.18)
cmake_policy(SET CMP0104 NEW)

project(GVirtuS LANGUAGES CXX)

# =========================================================================
# ==                   GLOBAL SETUP & DEPENDENCIES                       ==
# =========================================================================

# --- LZ4 check & link (Unchanged) ---
find_package(LZ4 QUIET)
if(LZ4_FOUND)
    link_libraries(LZ4::LZ4)
else()
    find_library(LZ4_LIB lz4 REQUIRED)
    link_libraries(${LZ4_LIB})
endif()

enable_testing()

# --- CUDA architectures (Unchanged) ---
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 75 80 86 89 90)
endif()

# --- C++ Standard (Unchanged) ---
set(CMAKE_CXX_STANDARD 17) # NOTE: Changed to 17, as 23 is still very new. 17 is sufficient for our code.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- GVIRTUS_HOME setup (Unchanged) ---
if("$ENV{GVIRTUS_HOME}" STREQUAL "")
    message(STATUS "Setting GVIRTUS_HOME=$ENV{HOME}/GVirtuS")
    set(GVIRTUS_HOME "$ENV{HOME}/GVirtuS")
    set($ENV{GVIRTUS_HOME} "${GVIRTUS_HOME}")
else()
    message(STATUS "Using GVIRTUS_HOME=$ENV{GVIRTUS_HOME}")
    set(GVIRTUS_HOME "$ENV{GVIRTUS_HOME}")
endif()

# --- Custom scripts and includes (Unchanged) ---
include(cmake/GVirtuS.cmake)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann/json.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann
    COMMAND ${CMAKE_COMMAND} -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann/json.hpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/download_json.cmake
)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)

# =========================================================================
# ==                        CORE LIBRARIES                               ==
# =========================================================================

# --- gvirtus-common (Unchanged) ---
add_library(gvirtus-common SHARED
    ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann/json.hpp
    src/common/Decoder.cpp
    src/common/Encoder.cpp
    src/common/JSON.cpp
    src/common/LD_Lib.cpp
    src/common/MessageDispatcher.cpp
    src/common/Mutex.cpp
    src/common/Observable.cpp
    src/common/Observer.cpp
    src/common/SignalException.cpp
    src/common/SignalState.cpp
    src/common/Util.cpp
)
target_link_libraries(gvirtus-common PRIVATE stdc++fs ${CMAKE_DL_LIBS} ${LIBLOG4CPLUS} rdmacm ibverbs)
gvirtus_install_target(gvirtus-common)
# Installation rules (Unchanged)
install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann DESTINATION ${GVIRTUS_HOME}/include)
if (LOG4CPLUS_EXTERNALLY_DOWNLOADED)
    add_dependencies(gvirtus-common log4cplus)
    install(DIRECTORY ${EXTERNAL_INSTALL_LOCATION}/include/log4cplus DESTINATION ${GVIRTUS_HOME}/include)
    install(DIRECTORY ${EXTERNAL_INSTALL_LOCATION}/lib DESTINATION ${GVIRTUS_HOME})
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/etc DESTINATION ${GVIRTUS_HOME})

# --- gvirtus-communicators (Modified to include our refactored files) ---
add_library(gvirtus-communicators SHARED
    # MODIFIED: These files have been updated for the async pipeline
    src/communicators/Buffer.cpp
    src/communicators/Result.cpp
    # UNCHANGED
    src/communicators/CommunicatorFactory.cpp
    src/communicators/Endpoint_Tcp.cpp
    src/communicators/Endpoint_Rdma.cpp
    src/communicators/Endpoint_Hybrid.cpp
    src/communicators/EndpointFactory.cpp
    src/communicators/rdma/ktmrdma.cpp
)
target_link_libraries(gvirtus-communicators PRIVATE gvirtus-common rdmacm ibverbs)
gvirtus_install_target(gvirtus-communicators)

# =========================================================================
# ==                 COMMUNICATOR IMPLEMENTATIONS                        ==
# =========================================================================

# --- TCP COMMUNICATOR PLUGIN (Final Corrected Version) ---
add_library(gvirtus-communicators-tcp SHARED
    src/communicators/tcp/TcpCommunicator.cpp
    src/communicators/tcp/EventLoop.cpp
    src/communicators/tcp/AsyncTcpCommunicator.cpp
    src/communicators/tcp/SyncAdapterCommunicator.cpp
)

# *** THIS IS THE FINAL AND MOST ROBUST CONFIGURATION ***
# By making these include directories PUBLIC, we ensure that any file
# including a header from this target (like AsyncTcpCommunicator.h) will also
# have access to these paths, thus finding Endpoint.h and IAsyncCommunicator.h.
target_include_directories(gvirtus-communicators-tcp
    PUBLIC
        # Provides IAsyncCommunicator.h, Endpoint.h, Endpoint_Tcp.h etc.
        ${PROJECT_SOURCE_DIR}/include 

        # Provides EventLoop.h etc. for the target's own sources
        ${PROJECT_SOURCE_DIR}/src/communicators/tcp
)

target_link_libraries(gvirtus-communicators-tcp 
    PUBLIC # Make this PUBLIC so the dependency is transitive
        gvirtus-communicators
    PRIVATE
        Threads::Threads
)

gvirtus_install_target(gvirtus-communicators-tcp)
# --- IB COMMUNICATOR (Unchanged) ---
add_library(gvirtus-communicators-ib SHARED
    src/communicators/rdma/ktmrdma.cpp
    src/communicators/rdma/RdmaCommunicator.cpp
)
target_link_libraries(gvirtus-communicators-ib PRIVATE gvirtus-communicators rdmacm ibverbs)
gvirtus_install_target(gvirtus-communicators-ib)

# --- HYBRID COMMUNICATOR (Unchanged) ---
add_library(gvirtus-communicators-hybrid SHARED
    src/communicators/hybrid/HybridCommunicator.cpp
)
target_include_directories(gvirtus-communicators-hybrid PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gvirtus-communicators-hybrid 
    PRIVATE
        gvirtus-communicators
        gvirtus-communicators-tcp # It depends on the TCP communicator
        gvirtus-communicators-ib
)
gvirtus_install_target(gvirtus-communicators-hybrid)

# =========================================================================
# ==                         FRONTEND & BACKEND                          ==
# =========================================================================

# --- FRONTEND (Unchanged, but now links to the new TCP lib) ---
add_library(gvirtus-frontend SHARED
    # MODIFIED: This file now contains the async/sync branching logic
    src/frontend/Frontend.cpp
)
target_include_directories(gvirtus-frontend PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gvirtus-frontend
    PRIVATE
        gvirtus-communicators
        gvirtus-communicators-hybrid # This implicitly links tcp
        Threads::Threads
        lz4
)
gvirtus_install_target(gvirtus-frontend)

# --- BACKEND (Unchanged, but now links to the new TCP lib) ---
add_executable(gvirtus-backend
    # MODIFIED: Process.cpp is now the core of the async pipeline
    src/backend/Process.cpp
    # UNCHANGED
    src/backend/Backend.cpp
    src/backend/main.cpp
    src/backend/Property.cpp
)
target_include_directories(gvirtus-backend PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gvirtus-backend
    PRIVATE
        gvirtus-communicators
        gvirtus-communicators-hybrid # This implicitly links tcp
        Threads::Threads
        rdmacm
        ibverbs
)
gvirtus_install_target(gvirtus-backend)

# =========================================================================
# ==                      PLUGINS & TESTS (Unchanged)                    ==
# =========================================================================

add_subdirectory(plugins/cudart)
add_subdirectory(plugins/cudadr)
add_subdirectory(plugins/cudnn) 

maybe_add_cuda_plugin(cublas plugins/cublas)
maybe_add_cuda_plugin(cufft plugins/cufft)
maybe_add_cuda_plugin(cusolver plugins/cusolver)
maybe_add_cuda_plugin(cusparse plugins/cusparse)
maybe_add_cuda_plugin(curand plugins/curand)
maybe_add_cuda_plugin(nvrtc plugins/nvrtc)

add_subdirectory(plugins/nvml)

add_subdirectory(tests)