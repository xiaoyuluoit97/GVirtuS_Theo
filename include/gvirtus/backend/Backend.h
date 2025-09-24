#pragma once

#include <string>
#include <vector>
#include <memory>

// *** CORRECTION 1: Use the experimental filesystem to match the rest of the project ***
#include <experimental/filesystem>

#include <gvirtus/common/Observer.h>
#include "Process.h"
#include "Property.h"
#include "log4cplus/logger.h"

// *** CORRECTION 2: Define the alias to the experimental namespace ***
namespace fs = std::experimental::filesystem;

namespace gvirtus::backend {

class Backend : public common::Observer {
public:
    explicit Backend(const fs::path &path);
    ~Backend() override;
    void Start();
    void EventOccurred(std::string &event, void *object) override;

private:
    Property _properties;
    fs::path m_path;
    log4cplus::Logger logger;
    int activeChilds = 0;
};

} // namespace gvirtus::backend