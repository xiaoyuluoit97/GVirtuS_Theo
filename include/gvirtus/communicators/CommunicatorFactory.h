#pragma once

#include <gvirtus/common/LD_Lib.h>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <algorithm> // for std::find
#include <stdexcept> // for std::runtime_error
#include <stdlib.h> /* getenv */

#include "Communicator.h"
#include "IAsyncCommunicator.h" // <--- new include for async interface
#include "Endpoint.h"
// Endpoint_Tcp.h and Endpoint_Rdma.h are likely not needed here anymore

namespace gvirtus::communicators {
    class CommunicatorFactory {
    public:
        /**
         * [Public Interface for OLD code]
         * Creates a Communicator instance compatible with the synchronous interface.
         * This remains for backward compatibility (e.g., for the Frontend).
         */
        static
        std::shared_ptr<common::LD_Lib<Communicator, std::shared_ptr<Endpoint>>>
        get_communicator(
                std::shared_ptr<Endpoint> end,
                bool secure = false
        ) {
            // It now calls the generic implementation with specific sync parameters.
            return get_communicator_impl<Communicator>(end, "create_communicator", secure);
        }

        /**
         * [Public Interface for NEW code]
         * Creates an IAsyncCommunicator instance compatible with the new asynchronous interface.
         * This should be used by the new backend pipeline.
         */
        static
        std::shared_ptr<common::LD_Lib<IAsyncCommunicator, std::shared_ptr<Endpoint>>>
        get_async_communicator(
                std::shared_ptr<Endpoint> end,
                bool secure = false
        ) {
            // It calls the generic implementation with specific async parameters.
            return get_communicator_impl<IAsyncCommunicator>(end, "create_async_communicator", secure);
        }

    private:
        /**
         * [Private Generic Implementation]
         * A templatized helper function that contains the core logic for loading
         * any type of communicator from a .so file.
         */
        template <typename InterfaceType>
        static
        std::shared_ptr<common::LD_Lib<InterfaceType, std::shared_ptr<Endpoint>>>
        get_communicator_impl(
                std::shared_ptr<Endpoint> end,
                const std::string& creator_symbol, // e.g., "create_communicator" or "create_async_communicator"
                bool secure = false
        ) {
#ifdef DEBUG
            std::cout << "CommunicatorFactory::get_communicator_impl() for symbol '" << creator_symbol << "' called." << std::endl;
            if(end) std::cout << "  Endpoint is: " << end->to_string() << std::endl;
#endif
            if (!end) {
                throw std::runtime_error("Endpoint cannot be null.");
            }

            // The logic for finding .so files and checking protocols remains the same.
            std::string gvirtus_home = getGVirtuSHome();
            validate_protocol(end->protocol(), secure);

            std::string dl_string = gvirtus_home + "/lib/libgvirtus-communicators-" + end->protocol() + ".so";

#ifdef DEBUG
            std::cout << "  Loading library: " << dl_string << std::endl;
#endif
            auto dl = std::make_shared<common::LD_Lib<InterfaceType, std::shared_ptr<Endpoint>>>(dl_string, creator_symbol);
            
            dl->build_obj(end);

#ifdef DEBUG
            std::cout << "  Successfully created communicator object from symbol '" << creator_symbol << "'." << std::endl;
#endif
            return dl;
        }

        static void validate_protocol(const std::string& protocol, bool secure) {
            // This logic is extracted for reuse.
            std::vector<std::string> unsecureMatches = {"tcp", "http", "oldtcp", "ws", "ib", "hybrid"};
            std::vector<std::string> secureMatches = {"https", "wss"};

            const auto& matches = secure ? secureMatches : unsecureMatches;
            if (std::find(matches.begin(), matches.end(), protocol) == matches.end()) {
                throw std::runtime_error((secure ? "Secure" : "Unsecure") + std::string(" communicator protocol not supported: ") + protocol);
            }
        }

        // Helper functions remain the same
        static std::string getEnvVar(std::string const &key) {
            char *val = getenv(key.c_str());
            return val == NULL ? std::string("") : std::string(val);
        }

        static std::string getGVirtuSHome() {
            std::string gvirtus_home = getEnvVar("GVIRTUS_HOME");
            if (gvirtus_home.empty()) {
                // Provide a fallback or throw an error if GVIRTUS_HOME is critical
                std::cerr << "Warning: GVIRTUS_HOME environment variable is not set." << std::endl;
            }
            return gvirtus_home;
        }

        // getConfigFile is not used by the factory logic itself, so it can remain as is.
        static std::string getConfigFile() {
            std::string config_path = getEnvVar("GVIRTUS_CONFIG");
            if (config_path.empty()) {
                std::string gvirtus_home = getGVirtuSHome();
                if (!gvirtus_home.empty()) {
                    config_path = gvirtus_home + "/etc/properties.json";
                } else {
                    config_path = "./properties.json";
                }
            }
            return config_path;
        }
    };
}  // namespace gvirtus::communicators