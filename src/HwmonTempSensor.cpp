
/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <unistd.h>

#include <HwmonTempSensor.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// The pressure units we receive are kilopascal.
// From: https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-bus-iio
// Units after application of scale and offset are kilopascal.
// 1 kilopascal = 10 hectopascal = 1000 Pascals
// The standard unit for used in atmospheric pressure measurements or
// readings is the hectopascal (hPa), in meteorology, for atmospheric
// pressure, the modern equivalent of the traditional millibar.
// However we tend to not put prefixes on dbus APIs, so our pressure
// is in Pascals.

static constexpr double maxReadingPressure = 120000; // Pascals
static constexpr double minReadingPressure = 30000;  // Pascals

static constexpr double maxReadingTemperature = 127;  // DegreesC
static constexpr double minReadingTemperature = -128; // DegreesC

HwmonTempSensor::HwmonTempSensor(
    const std::string& path, const std::string& objectType,
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_service& io, const std::string& sensorName,
    std::vector<thresholds::Threshold>&& thresholdsIn, const double offsetValue,
    const double scaleValue, const std::string& units, const float pollRate,
    const std::string& sensorConfiguration, const PowerState powerState,
    const std::string& sensorType) :
    Sensor(boost::replace_all_copy(sensorName, " ", "_"),
           std::move(thresholdsIn), sensorConfiguration, objectType, false,
           sensorType.compare("pressure") ? maxReadingTemperature
                                          : maxReadingPressure,
           sensorType.compare("pressure") ? minReadingTemperature
                                          : minReadingPressure,
           conn, powerState),
    std::enable_shared_from_this<HwmonTempSensor>(), objServer(objectServer),
    inputDev(io, open(path.c_str(), O_RDONLY)), waitTimer(io), path(path),
    offsetValue(offsetValue), scaleValue(scaleValue), units(units),
    sensorPollMs(static_cast<unsigned int>(pollRate * 1000))
{
    sensorInterface = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/" + sensorType + "/" + name,
        "xyz.openbmc_project.Sensor.Value");

    if (thresholds::hasWarningInterface(thresholds))
    {
        thresholdInterfaceWarning = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/" + sensorType + "/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Warning");
    }
    if (thresholds::hasCriticalInterface(thresholds))
    {
        thresholdInterfaceCritical = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/" + sensorType + "/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Critical");
    }
    association = objectServer.add_interface("/xyz/openbmc_project/sensors/" +
                                                 sensorType + "/" + name,
                                             association::interface);
    setInitialProperties(conn, units);
}

HwmonTempSensor::~HwmonTempSensor()
{
    // close the input dev to cancel async operations
    inputDev.close();
    waitTimer.cancel();
    objServer.remove_interface(thresholdInterfaceWarning);
    objServer.remove_interface(thresholdInterfaceCritical);
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
}

void HwmonTempSensor::setupRead(void)
{
    if (!readingStateGood())
    {
        markAvailable(false);
        updateValue(std::numeric_limits<double>::quiet_NaN());
        restartRead();
        return;
    }

    std::weak_ptr<HwmonTempSensor> weakRef = weak_from_this();
    boost::asio::async_read_until(inputDev, readBuf, '\n',
                                  [weakRef](const boost::system::error_code& ec,
                                            std::size_t /*bytes_transfered*/) {
                                      std::shared_ptr<HwmonTempSensor> self =
                                          weakRef.lock();
                                      if (self)
                                      {
                                          self->handleResponse(ec);
                                      }
                                  });
}

void HwmonTempSensor::restartRead()
{
    std::weak_ptr<HwmonTempSensor> weakRef = weak_from_this();
    waitTimer.expires_from_now(boost::posix_time::milliseconds(sensorPollMs));
    waitTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        std::shared_ptr<HwmonTempSensor> self = weakRef.lock();
        if (!self)
        {
            return;
        }
        self->setupRead();
    });
}

void HwmonTempSensor::handleResponse(const boost::system::error_code& err)
{
    if ((err == boost::system::errc::bad_file_descriptor) ||
        (err == boost::asio::error::misc_errors::not_found))
    {
        std::cerr << "Hwmon temp sensor " << name << " removed " << path
                  << "\n";
        return; // we're being destroyed
    }
    std::istream responseStream(&readBuf);
    if (!err)
    {
        std::string response;
        std::getline(responseStream, response);
        try
        {
            double nvalue;
            rawValue = std::stod(response);
            nvalue = (rawValue + offsetValue) * scaleValue;
            updateValue(nvalue);
        }
        catch (const std::invalid_argument&)
        {
            incrementError();
        }
    }
    else
    {
        incrementError();
    }

    responseStream.clear();
    inputDev.close();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        std::cerr << "Hwmon temp sensor " << name << " not valid " << path
                  << "\n";
        return; // we're no longer valid
    }
    inputDev.assign(fd);
    restartRead();
}

void HwmonTempSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}
