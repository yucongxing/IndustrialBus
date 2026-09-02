#pragma once

#include "simple_logger.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace simulated_apps {

std::string defaultCommHubExecutable();

struct LayoutViewOptions {
    std::string hub_executable;
    std::chrono::seconds timeout{15};
};

struct RunSimulationOptions {
    std::string input_uri{"file:///tmp/layoutview-simulated-layout.oas"};
    std::size_t steps{5};
};

class LayoutViewClient {
public:
    virtual ~LayoutViewClient() = default;
    virtual bool start(std::string& error) = 0;
    virtual bool runSimulation(const RunSimulationOptions& options,
                               std::string& result_uri,
                               std::string& error) = 0;
    virtual void stop() noexcept = 0;
};

std::unique_ptr<LayoutViewClient> makeLayoutViewClient(
    LayoutViewOptions options,
    std::shared_ptr<Logger> logger);

struct CmpSuiteOptions {
    std::string hub_executable;
    std::string journal_path;
};

class SimulationJob {
public:
    virtual ~SimulationJob() = default;
    virtual const std::string& operationId() const noexcept = 0;
    virtual const std::string& inputUri() const noexcept = 0;
    virtual std::size_t requestedSteps() const noexcept = 0;
    virtual bool reportProgress(std::size_t current,
                                std::size_t total,
                                std::string stage,
                                std::string message) = 0;
    virtual bool complete(std::string result_uri) = 0;
    virtual bool fail(std::string message) = 0;
};

using SimulationHandler =
    std::function<void(const std::shared_ptr<SimulationJob>&)>;

class CmpSuiteService {
public:
    virtual ~CmpSuiteService() = default;
    virtual bool start(SimulationHandler handler, std::string& error) = 0;
    virtual void stop() noexcept = 0;
};

std::unique_ptr<CmpSuiteService> makeCmpSuiteService(
    CmpSuiteOptions options,
    std::shared_ptr<Logger> logger);

}  // namespace simulated_apps
