#pragma once

#include "PwmSensor.hpp"
#include "Thresholds.hpp"
#include "sensor.hpp"

#include <unistd.h>

#include <memory>
#include <sdbusplus/asio/object_server.hpp>
#include <string>

enum PSUDisposition
{
    DISPOSITION_NEW = 0,
    DISPOSITION_SLOW,
    DISPOSITION_BAD,
    DISPOSITION_GOOD
};

class PSUSensor : public Sensor
{
  public:
    PSUSensor(const std::string& path, const std::string& objectType,
              sdbusplus::asio::object_server& objectServer,
              std::shared_ptr<sdbusplus::asio::connection>& conn,
              boost::asio::io_service& io, const std::string& sensorName,
              std::vector<thresholds::Threshold>&& thresholds,
              const std::string& sensorConfiguration,
              std::string& sensorTypeName, unsigned int factor, double max,
              double min, const std::string& label, size_t tSize);
    ~PSUSensor();

    PSUDisposition prepareInput();

  private:
    sdbusplus::asio::object_server& objServer;
    boost::asio::posix::stream_descriptor inputStream;

    // Limit streambuf size to prevent buggy sensor from endless loop reading
    boost::asio::streambuf inputBuf{static_cast<size_t>(getpagesize())};

    std::string path;
    size_t errCount;
    size_t slowCount;
    size_t readCount;
    size_t goodCount;
    unsigned int sensorFactor;
    PSUDisposition disposition;
    bool readPending;

    void handleResponse(const boost::system::error_code& err);
    void checkThresholds(void) override;

    static constexpr size_t warnAfterErrorCount = 10;
};

class PSUProperty
{
  public:
    PSUProperty(std::string name, double max, double min, unsigned int factor) :
        labelTypeName(name), maxReading(max), minReading(min),
        sensorScaleFactor(factor)
    {
    }
    ~PSUProperty() = default;

    std::string labelTypeName;
    double maxReading;
    double minReading;
    unsigned int sensorScaleFactor;
};
