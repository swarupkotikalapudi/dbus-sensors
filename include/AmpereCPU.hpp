#pragma once

#include <Thresholds.hpp>
#include <boost/asio/streambuf.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>

#include <memory>
#include <string>
#include <utility>

class AmpereCPUSensor :
    public Sensor,
    public std::enable_shared_from_this<AmpereCPUSensor>
{
  public:
    AmpereCPUSensor(const std::string& path, const std::string& objectType,
                    sdbusplus::asio::object_server& objectServer,
                    std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_service& io, const std::string& sensorName,
                    std::vector<thresholds::Threshold>&& thresholds,
                    const std::string& sensorConfiguration,
                    std::string& sensorTypeName, double factor, double max,
                    double min, const std::string& label, size_t tSize,
                    PowerState readState);
    ~AmpereCPUSensor() override;
    void setupRead(void);

  private:
    sdbusplus::asio::object_server& objServer;
    boost::asio::posix::stream_descriptor inputDev;
    boost::asio::deadline_timer waitTimer;
    std::shared_ptr<boost::asio::streambuf> readBuf;
    std::string path;
    double sensorFactor;
    void handleResponse(const boost::system::error_code& err);
    void checkThresholds(void) override;

    int fd;
    static constexpr unsigned int sensorPollMs = 1000;
    static constexpr size_t warnAfterErrorCount = 10;
};

class AmpereCPUProperty
{
  public:
    AmpereCPUProperty(std::string name, double max, double min, double factor) :
        labelTypeName(std::move(name)), maxReading(max), minReading(min),
        sensorScaleFactor(factor)
    {}
    ~AmpereCPUProperty() = default;

    std::string labelTypeName;
    double maxReading;
    double minReading;
    double sensorScaleFactor;
};
