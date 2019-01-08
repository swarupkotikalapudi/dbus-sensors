#pragma once
#include "sensor.hpp"

#include <boost/asio/deadline_timer.hpp>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <limits>
#include <vector>

enum class IpmbType
{
    meSensor,
    infineonVR,
};

struct IpmbSensor : public Sensor
{
    IpmbSensor(std::shared_ptr<sdbusplus::asio::connection> &conn,
               boost::asio::io_service &io, const std::string &name,
               const std::string &sensorConfiguration,
               sdbusplus::asio::object_server &objectServer,
               std::vector<thresholds::Threshold> &&thresholds);
    ~IpmbSensor();

    void checkThresholds(void) override;
    void read(void);
    void init(void);

    IpmbType type;
    uint8_t commandAddress;
    uint8_t netfn;
    uint8_t command;
    std::vector<uint8_t> commandData;
    std::optional<uint8_t> initCommand;
    std::vector<uint8_t> initData;

    // to date all ipmb sensors are power on only
    PowerState readState = PowerState::on;

  private:
    sdbusplus::asio::object_server &objectServer;
    std::shared_ptr<sdbusplus::asio::connection> dbusConnection;
    boost::asio::deadline_timer waitTimer;
};