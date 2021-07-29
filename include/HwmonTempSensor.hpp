#pragma once

#include <Thresholds.hpp>
#include <boost/asio/streambuf.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>

#include <string>
#include <vector>

class HwmonTempSensor :
    public Sensor,
    public std::enable_shared_from_this<HwmonTempSensor>
{
  public:
    HwmonTempSensor(const std::string& path, const std::string& objectType,
                    sdbusplus::asio::object_server& objectServer,
                    std::shared_ptr<sdbusplus::asio::connection>& conn,
                    boost::asio::io_service& io, const std::string& sensorName,
                    std::vector<thresholds::Threshold>&& thresholds,
                    const double offsetValue, const double scaleValue,
                    const double minValue, const double maxValue,
                    const std::string& units, const float pollRate,
                    const std::string& sensorConfiguration,
                    const PowerState powerState, const std::string& sensorType);
    ~HwmonTempSensor() override;
    void setupRead(void);

  private:
    sdbusplus::asio::object_server& objServer;
    boost::asio::posix::stream_descriptor inputDev;
    boost::asio::deadline_timer waitTimer;
    boost::asio::streambuf readBuf;
    std::string path;
    double offsetValue;
    double scaleValue;
    double minValue;
    double maxValue;
    std::string units;
    unsigned int sensorPollMs;

    void handleResponse(const boost::system::error_code& err);
    void restartRead();
    void checkThresholds(void) override;
};
