/*
// Copyright (c) 2018 Intel Corporation
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

#include "ChassisIntrusionSensor.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Utils.hpp>
#include <boost/asio/io_context.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

static constexpr bool debug = false;

static constexpr unsigned int intrusionSensorPollSec = 1;

// SMLink Status Register
const static constexpr size_t pchStatusRegIntrusion = 0x04;

// Status bit field masks
const static constexpr size_t pchRegMaskIntrusion = 0x01;

// Value to clear intrusion status hwmon file
const static constexpr size_t intrusionStatusHwmonClearValue = 0;

void ChassisIntrusionSensor::updateValue(const std::string& newValue)
{
    // Take no action if value already equal
    // Same semantics as Sensor::updateValue(const double&)
    if (newValue == mValue)
    {
        return;
    }

    // indicate that it is internal set call
    mInternalSet = true;
    mIface->set_property("Status", newValue);
    mInternalSet = false;

    mValue = newValue;

    if (mOldValue == "Normal" && mValue != "Normal")
    {
        std::cerr << "save to SEL for intrusion assert event \n";
        // TODO: call add SEL log API, depends on patch #13956
        mOldValue = mValue;
    }
    else if (mOldValue != "Normal" && mValue == "Normal")
    {
        std::cerr << "save to SEL for intrusion de-assert event \n";
        // TODO: call add SEL log API, depends on patch #13956
        mOldValue = mValue;
    }
}

void ChassisIntrusionSensor::i2cReadFromPch()
{
    std::string i2cBus = "/dev/i2c-" + std::to_string(mBusId);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(i2cBus.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "unable to open i2c device \n";
        return;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(fd, I2C_SLAVE_FORCE, mSlaveAddr) < 0)
    {
        std::cerr << "unable to set device address\n";
        close(fd);
        return;
    }

    unsigned long funcs = 0;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (ioctl(fd, I2C_FUNCS, &funcs) < 0)
    {
        std::cerr << "not support I2C_FUNCS \n";
        close(fd);
        return;
    }

    if ((funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA) == 0U)
    {
        std::cerr << "not support I2C_FUNC_SMBUS_READ_BYTE_DATA \n";
        close(fd);
        return;
    }

    int32_t statusMask = pchRegMaskIntrusion;
    int32_t statusReg = pchStatusRegIntrusion;

    int32_t statusValue = i2c_smbus_read_byte_data(fd, statusReg);
    if constexpr (debug)
    {
        std::cout << "\nRead bus " << mBusId << " addr " << mSlaveAddr
                  << ", value = " << statusValue << "\n";
    }

    close(fd);

    if (statusValue < 0)
    {
        std::cerr << "i2c_smbus_read_byte_data failed \n";
        return;
    }

    // Get status value with mask
    int value = statusValue & statusMask;

    if constexpr (debug)
    {
        std::cout << "statusValue is " << statusValue << "\n";
        std::cout << "Intrusion sensor value is " << value << "\n";
    }

    std::string newValue = value != 0 ? "HardwareIntrusion" : "Normal";

    if (mValue != newValue)
    {
        std::cout << "update value from " << mValue << " to " << newValue
                  << "\n";
        updateValue(newValue);
    }
    // trigger next polling
    pollSensorStatusByPch();
}

void ChassisIntrusionSensor::pollSensorStatusByPch()
{
    std::weak_ptr<ChassisIntrusionSensor> weakRef = weak_from_this();
    // setting a new experation implicitly cancels any pending async wait
    mPchPollTimer.expires_after(std::chrono::seconds(intrusionSensorPollSec));

    mPchPollTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        std::shared_ptr<ChassisIntrusionSensor> self = weakRef.lock();
        // case of being canceled
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Timer of intrusion sensor is cancelled. Return \n";
            return;
        }
        if (self)
        {
            self->i2cReadFromPch();
        }
        else
        {
            std::cerr << "ChassisIntrusionSensor no self\n";
        }
    });
}

void ChassisIntrusionSensor::readGpio()
{
    mGpioLine.event_read();
    auto value = mGpioLine.get_value();

    // set string defined in chassis redfish schema
    std::string newValue = value != 0 ? "HardwareIntrusion" : "Normal";

    if constexpr (debug)
    {
        std::cout << "\nGPIO value is " << value << "\n";
        std::cout << "Intrusion sensor value is " << newValue << "\n";
    }

    if (mValue != newValue)
    {
        std::cout << "update value from " << mValue << " to " << newValue
                  << "\n";
        updateValue(newValue);
    }
    pollSensorStatusByGpio();
}

void ChassisIntrusionSensor::pollSensorStatusByGpio()
{
    mGpioFd.async_wait(boost::asio::posix::stream_descriptor::wait_read,
                       [this](const boost::system::error_code& ec) {
        if (ec == boost::system::errc::bad_file_descriptor)
        {
            return; // we're being destroyed
        }
        if (ec)
        {
            std::cerr << "Error on GPIO based intrusion sensor wait event\n";
        }
        else
        {
            readGpio();
        }
    });
}

void ChassisIntrusionSensor::initGpioDeviceFile()
{
    mGpioLine = gpiod::find_line(mPinName);
    if (!mGpioLine)
    {
        std::cerr << "ChassisIntrusionSensor error finding gpio pin name: "
                  << mPinName << "\n";
        return;
    }

    try
    {

        mGpioLine.request(
            {"ChassisIntrusionSensor", gpiod::line_request::EVENT_BOTH_EDGES,
             mGpioInverted ? gpiod::line_request::FLAG_ACTIVE_LOW : 0});

        // set string defined in chassis redfish schema
        auto value = mGpioLine.get_value();
        std::string newValue = value != 0 ? "HardwareIntrusion" : "Normal";
        updateValue(newValue);

        auto gpioLineFd = mGpioLine.event_get_fd();
        if (gpioLineFd < 0)
        {
            std::cerr << "ChassisIntrusionSensor failed to get " << mPinName
                      << " fd\n";
            return;
        }

        mGpioFd.assign(gpioLineFd);
    }
    catch (const std::system_error& e)
    {
        std::cerr << "ChassisIntrusionSensor error requesting gpio pin name: "
                  << mPinName << " : " << e.what() << "\n";
        return;
    }
}

void ChassisIntrusionSensor::readHwmon()
{
    std::ifstream refin(mHwmonPath);
    if (!refin.good())
    {
        std::cerr << "Error reading status at " << mHwmonPath << "\n";
        return;
    }
    std::string line;
    if (!std::getline(refin, line))
    {
        std::cerr << "Error reading status at " << mHwmonPath << "\n";
        return;
    }
    try
    {
        size_t value = std::stoi(line);

        // set string defined in chassis redfish schema
        std::string newValue = value != 0 ? "HardwareIntrusion" : "Normal";

        if constexpr (debug)
        {
            std::cout << "Hwmon value is " << std::dec << value << "\n";
        }
        if (mValue != newValue)
        {
            std::cout << "update value from " << mValue << " to " << newValue
                      << "\n";
            updateValue(newValue);
        }
    }
    catch (const std::invalid_argument& e)
    {
        std::cerr << "Error reading status at " << mHwmonPath << " : "
                  << e.what() << "\n";
    }
    // Reset chassis intrusion status after every reading
    std::ofstream refout(mHwmonPath);
    if (!refout.good())
    {
        std::cerr << "Error resetting intrusion status at " << mHwmonPath
                  << "\n";
        return;
    }
    refout << intrusionStatusHwmonClearValue;
    pollSensorStatusByHwmon();
}

void ChassisIntrusionSensor::pollSensorStatusByHwmon()
{
    std::weak_ptr<ChassisIntrusionSensor> weakRef = weak_from_this();
    // setting a new experation implicitly cancels any pending async wait
    mHwmonPollTimer.expires_after(std::chrono::seconds(intrusionSensorPollSec));

    mHwmonPollTimer.async_wait([weakRef](const boost::system::error_code& ec) {
        std::shared_ptr<ChassisIntrusionSensor> self = weakRef.lock();
        // timer expired
        if (ec == boost::asio::error::operation_aborted)
        {
            std::cerr << "Timer of hwmon-based intrusion sensor"
                      << " is cancelled. Return \n";
            return;
        }
        if (self)
        {
            self->readHwmon();
        }
        else
        {
            std::cerr << "ChassisIntrusionSensor no self\n";
        }
    });
}

void ChassisIntrusionSensor::initHwmonDevicePath()
{
    namespace fs = std::filesystem;
    std::vector<fs::path> paths;
    if (!findFiles(fs::path("/sys/class/hwmon"), mHwmonName, paths))
    {
        std::cerr << "No intrusion status found in system\n";
        return;
    }

    if (paths.empty())
    {
        std::cerr << "ChassisIntrusionSensor failed to get chassis intrusion "
                     "status path\n";
        return;
    }

    if (paths.size() > 1)
    {
        std::cerr << "Found more than 1 hwmon file to read chassis intrusion"
                  << " status. Taking the first one. \n";
    }

    mHwmonPath = paths[0].string();
    if constexpr (debug)
    {
        std::cout << "Found " << paths.size()
                  << " paths for intrusion status \n"
                  << " The first path is: " << mHwmonPath << "\n";
    }
}

int ChassisIntrusionSensor::setSensorValue(const std::string& req,
                                           std::string& propertyValue)
{
    if (!mInternalSet)
    {
        propertyValue = req;
        mOverridenState = true;
    }
    else if (!mOverridenState)
    {
        propertyValue = req;
    }
    return 1;
}

void ChassisIntrusionSensor::start(IntrusionSensorType type, int busId,
                                   int slaveAddr, bool gpioInverted)
{
    if constexpr (debug)
    {
        std::cerr << "enter ChassisIntrusionSensor::start, type = " << type
                  << "\n";
        if (type == IntrusionSensorType::pch)
        {
            std::cerr << "busId = " << busId << ", slaveAddr = " << slaveAddr
                      << "\n";
        }
        else if (type == IntrusionSensorType::gpio)
        {
            std::cerr << "gpio pinName = " << mPinName
                      << ", gpioInverted = " << gpioInverted << "\n";
        }
    }

    if ((type == IntrusionSensorType::pch && busId == mBusId &&
         slaveAddr == mSlaveAddr) ||
        (type == IntrusionSensorType::gpio && gpioInverted == mGpioInverted &&
         mInitialized) ||
        (type == IntrusionSensorType::hwmon && mInitialized))
    {
        return;
    }

    mType = type;
    mBusId = busId;
    mSlaveAddr = slaveAddr;
    mGpioInverted = gpioInverted;

    if ((mType == IntrusionSensorType::pch && mBusId > 0 && mSlaveAddr > 0) ||
        (mType == IntrusionSensorType::gpio) ||
        (mType == IntrusionSensorType::hwmon))
    {
        // initialize first if not initialized before
        if (!mInitialized)
        {
            mIface->register_property(
                "Status", mValue,
                [&](const std::string& req, std::string& propertyValue) {
                return setSensorValue(req, propertyValue);
                });
            mIface->initialize();

            if (mType == IntrusionSensorType::gpio)
            {
                initGpioDeviceFile();
            }
            else if (mType == IntrusionSensorType::hwmon)
            {
                initHwmonDevicePath();
            }

            mInitialized = true;
        }

        // start polling value
        if (mType == IntrusionSensorType::pch)
        {
            pollSensorStatusByPch();
        }
        else if (mType == IntrusionSensorType::gpio && mGpioLine)
        {
            std::cerr << "Start polling gpio based intrusion sensors\n";
            pollSensorStatusByGpio();
        }
        else if (mType == IntrusionSensorType::hwmon && !mHwmonPath.empty())
        {
            std::cerr << "Start polling hwmon based intrusion sensors\n";
            pollSensorStatusByHwmon();
        }
    }

    // invalid para, release resource
    else
    {
        if (mInitialized)
        {
            if (mType == IntrusionSensorType::pch)
            {
                mPchPollTimer.cancel();
            }
            else if (mType == IntrusionSensorType::hwmon)
            {
                mHwmonPollTimer.cancel();
            }
            else if (mType == IntrusionSensorType::gpio)
            {
                mGpioFd.close();
                if (mGpioLine)
                {
                    mGpioLine.release();
                }
            }
            mInitialized = false;
        }
    }
}

ChassisIntrusionSensor::ChassisIntrusionSensor(
    boost::asio::io_context& io,
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface) :
    mIface(std::move(iface)),
    mValue("unknown"), mOldValue("unknown"), mPchPollTimer(io), mGpioFd(io),
    mHwmonPollTimer(io)
{}

ChassisIntrusionSensor::~ChassisIntrusionSensor()
{
    if (mType == IntrusionSensorType::pch)
    {
        mPchPollTimer.cancel();
    }
    else if (mType == IntrusionSensorType::hwmon)
    {
        mHwmonPollTimer.cancel();
    }
    else if (mType == IntrusionSensorType::gpio)
    {
        mGpioFd.close();
        if (mGpioLine)
        {
            mGpioLine.release();
        }
    }
}
