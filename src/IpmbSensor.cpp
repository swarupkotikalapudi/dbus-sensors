/*
// Copyright (c) 2019 Intel Corporation
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

#include <IpmbSDRSensor.hpp>
#include <IpmbSensor.hpp>
#include <Utils.hpp>
#include <VariantVisitors.hpp>
#include <boost/container/flat_map.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

constexpr const bool debug = false;

constexpr const char* sensorType = "IpmbSensor";
constexpr const char* sdrInterface = "IpmbDevice";

static constexpr double ipmbMaxReading = 0xFF;
static constexpr double ipmbMinReading = 0;

static constexpr uint8_t meAddress = 1;
static constexpr uint8_t lun = 0;
static constexpr uint8_t hostSMbusIndexDefault = 0x03;
static constexpr uint8_t ipmbBusIndexDefault = 0;
static constexpr float pollRateDefault = 1; // in seconds

static constexpr const char* sensorPathPrefix = "/xyz/openbmc_project/sensors/";

boost::container::flat_map<std::string, std::shared_ptr<IpmbSensor>> sensors;
boost::container::flat_map<uint8_t, std::shared_ptr<IpmbSDRDevice>> sdrsensor;

std::unique_ptr<boost::asio::steady_timer> initCmdTimer;

IpmbSensor::IpmbSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                       boost::asio::io_service& io,
                       const std::string& sensorName,
                       const std::string& sensorConfiguration,
                       sdbusplus::asio::object_server& objectServer,
                       std::vector<thresholds::Threshold>&& thresholdData,
                       uint8_t deviceAddress, uint8_t hostSMbusIndex,
                       const float pollRate, std::string& sensorTypeName) :
    Sensor(escapeName(sensorName), std::move(thresholdData),
           sensorConfiguration, "IpmbSensor", false, false, ipmbMaxReading,
           ipmbMinReading, conn, PowerState::on),
    deviceAddress(deviceAddress), hostSMbusIndex(hostSMbusIndex),
    sensorPollMs(static_cast<int>(pollRate * 1000)), objectServer(objectServer),
    waitTimer(io)
{
    std::string dbusPath = sensorPathPrefix + sensorTypeName + "/" + name;

    sensorInterface = objectServer.add_interface(
        dbusPath, "xyz.openbmc_project.Sensor.Value");

    for (const auto& threshold : thresholds)
    {
        std::string interface = thresholds::getInterface(threshold.level);
        thresholdInterfaces[static_cast<size_t>(threshold.level)] =
            objectServer.add_interface(dbusPath, interface);
    }
    association = objectServer.add_interface(dbusPath, association::interface);
}

IpmbSensor::~IpmbSensor()
{
    waitTimer.cancel();
    for (const auto& iface : thresholdInterfaces)
    {
        objectServer.remove_interface(iface);
    }
    objectServer.remove_interface(sensorInterface);
    objectServer.remove_interface(association);
}

std::string IpmbSensor::getSubTypeUnits(void) const
{
    switch (subType)
    {
        case IpmbSubType::temp:
            return sensor_paths::unitDegreesC;
        case IpmbSubType::curr:
            return sensor_paths::unitAmperes;
        case IpmbSubType::power:
            return sensor_paths::unitWatts;
        case IpmbSubType::volt:
            return sensor_paths::unitVolts;
        case IpmbSubType::util:
            return sensor_paths::unitPercent;
        default:
            throw std::runtime_error("Invalid sensor type");
    }
}

void IpmbSensor::init(void)
{
    loadDefaults();
    setInitialProperties(getSubTypeUnits());
    if (initCommand)
    {
        runInitCmd();
    }
    read();
}

static void initCmdCb(const std::weak_ptr<IpmbSensor>& weakRef,
                      const boost::system::error_code& ec,
                      const IpmbMethodType& response)
{
    std::shared_ptr<IpmbSensor> self = weakRef.lock();
    if (!self)
    {
        return;
    }
    const int& status = std::get<0>(response);
    if (ec || (status != 0))
    {
        std::cerr << "Error setting init command for device: " << self->name
                  << "\n";
    }
}

void IpmbSensor::runInitCmd()
{
    if (!initCommand)
    {
        return;
    }
    dbusConnection->async_method_call(
        [weakRef{weak_from_this()}](const boost::system::error_code& ec,
                                    const IpmbMethodType& response) {
        initCmdCb(weakRef, ec, response);
        },
        "xyz.openbmc_project.Ipmi.Channel.Ipmb",
        "/xyz/openbmc_project/Ipmi/Channel/Ipmb", "org.openbmc.Ipmb",
        "sendRequest", commandAddress, netfn, lun, *initCommand, initData);
}

/**
 * Reference:
 * Intelligent Power Node Manager External Interface Specification
 */
std::vector<uint8_t> IpmbSensor::getRawPmbusCommand(
    uint8_t messageType, const std::vector<uint8_t>& pmbusCommand,
    ipmi::sensor::PmbusResponseLength readLength,
    ipmi::sensor::AddressMode deviceAddressMode, bool doEnablePec)
{
    constexpr std::array<uint8_t, 3> manufacturerId{0x57, 0x01, 0x00};
    std::vector<uint8_t> commandBytes;

    if (std::numeric_limits<uint8_t>::max() < pmbusCommand.size())
    {
        throw std::runtime_error("Invalid pmbus command length");
    }

    /*
     * Byte 1:3 - Intel Manufacturer ID - 000157h, LS byte first.
     */
    commandBytes.insert(commandBytes.end(), manufacturerId.begin(),
                        manufacturerId.end());

    /*
     * Byte 4 - Flags
     * [0] - Reserved. Write as 0b.
     * [3:1] - SMBUS message transaction type
     * =0h - SEND_BYTE.
     * =1h - READ_BYTE.
     * =2h - WRITE_BYTE.
     * =3h - READ_WORD.
     * =4h - WRITE_WORD.
     * =5h - BLOCK_READ.
     * =6h - BLOCK_WRITE.
     * =7h - BLOCK_WRITE_READ_PROC_CALL.
     * [5:4] - Device address format.
     * =0h - Standard device address
     * =1h - Extended device address
     * Other - reserved.
     * [6] = 1b - Do not report PEC errors in Completion Code. If
     * the bit is set, Intel NM firmware does not return "bad PEC"
     * Completion Codes. In this case BMC can learn that the
     * transaction failed by checking PEC in the PMBUS response
     * message. The flag does not disable PMBUS command
     * retries.
     * [7] = 1b - Enable PEC.
     * For Standard device address (Byte 4 bits [5:4] equal 0)
     */
    uint8_t byte4 = 0x00;
    if (ipmi::sensor::AddressMode::elevenBit == deviceAddressMode)
    {
        byte4 |= (1 << 4);
    }

    if (doEnablePec)
    {
        byte4 |= (1 << 7);
    }

    byte4 |= static_cast<uint8_t>(messageType) << 1;

    commandBytes.emplace_back(byte4);

    if (ipmi::sensor::AddressMode::elevenBit == deviceAddressMode)
    {
        /*
         * Byte 5 - Sensor Bus
         * =00h SMBUS
         * =01h SMLINK0/SMLINK0B
         * =02h SMLINK1
         * =03h SMLINK2
         * =04h SMLINK3
         * =05h SMLINK4
         * Other - reserved
         */
        commandBytes.emplace_back(hostSMbusIndex);

        /*
         * Byte 6 - Target PSU Address
         * [0] - Reserved. Write as 0b.
         * [7:1] - 7-bit SMBUS address.
         */
        commandBytes.emplace_back(deviceAddress);

        /*
         * Byte 7 - MUX Address
         * [0] - Reserved. Write as 0b.
         * [7:1] - 7-bit SMBUS address for SMBUS MUX or 0 for
         * MGPIO controlled.
         */
        commandBytes.emplace_back(0x00);

        /*
         * Byte 8 - MUX channel selection
         * This field indicates which line of MUX should be enabled
         */
        commandBytes.emplace_back(0x00);

        /*
         * Byte 9 - MUX configuration state
         * [0] - MUX support
         * =0 - ignore MUX configuration (MUX not present)
         * =1 - use MUX configuration
         * [7:1] - Reserved. Write as 0000000b.
         */
        commandBytes.emplace_back(0x00);
    }
    else
    {
        /*
         * Byte 5 - Target PSU Address
         * [0] - reserved should be set to 0
         * [7:1] - 7-bit PSU SMBUS address
         */
        commandBytes.emplace_back(deviceAddress);

        /*
         * Byte 6 - MGPIO MUX configuration
         * [5:0] = Mux MGPIO index or 0 if mux is not used.
         * [7:6] = Reserved. Write as 00b.
         */
        commandBytes.emplace_back(0x00);
    }

    /*
     * Byte 7 or 10 - Transmission Protocol parameter
     * [4:0] = Reserved. Write as 00000b.
     * [5] = Transmission Protocol
     * =0b PMBus
     * =1b I2C (Raw I2C transaction without the command Field)
     * [7:6] = Reserved. Write as 00b.
     */
    commandBytes.emplace_back(0x00);

    /*
     * Byte 8 or 11 - Write Length
     */
    commandBytes.emplace_back(static_cast<uint8_t>(pmbusCommand.size()));

    /*
     * Byte 9 or 12 - Read Length. This field is used to validate if the
     * slave returns proper number of bytes
     */
    commandBytes.emplace_back(static_cast<uint8_t>(readLength));

    /*
     * Byte Byte 10 or 13 to M - PMBUS command
     * For Extended device address (Byte 4 bits [5:4] equal 1)
     */
    commandBytes.insert(commandBytes.end(), pmbusCommand.begin(),
                        pmbusCommand.end());

    return commandBytes;
}

void IpmbSensor::loadDefaults()
{
    if (type == IpmbType::meSensor)
    {
        commandAddress = meAddress;
        netfn = ipmi::sensor::netFn;
        command = ipmi::sensor::getSensorReading;
        commandData = {deviceAddress};
        readingFormat = ReadingFormat::byte0;
    }
    else if (type == IpmbType::PXE1410CVR)
    {
        commandAddress = meAddress;
        netfn = ipmi::me_bridge::netFn;
        command = ipmi::me_bridge::sendRawPmbus;
        initCommand = ipmi::me_bridge::sendRawPmbus;
        // pmbus read temp
        commandData = getRawPmbusCommand(
            ipmi::sensor::smbusMessageTypeReadWord,
            {static_cast<uint8_t>(ipmi::sensor::PmbusRequest::readTemperature)},
            ipmi::sensor::PmbusResponseLength::readTemperature,
            ipmi::sensor::AddressMode::elevenBit, false);
        // goto page 0
        initData = getRawPmbusCommand(
            ipmi::sensor::smbusMessageTypeWriteByte, {0x00, 0x00},
            ipmi::sensor::PmbusResponseLength::writeByte,
            ipmi::sensor::AddressMode::elevenBit, false);
        readingFormat = ReadingFormat::linearElevenBit;
    }
    else if (type == IpmbType::IR38363VR)
    {
        commandAddress = meAddress;
        netfn = ipmi::me_bridge::netFn;
        command = ipmi::me_bridge::sendRawPmbus;
        // pmbus read temp
        commandData = getRawPmbusCommand(
            ipmi::sensor::smbusMessageTypeReadWord,
            {static_cast<uint8_t>(ipmi::sensor::PmbusRequest::readTemperature)},
            ipmi::sensor::PmbusResponseLength::readTemperature,
            ipmi::sensor::AddressMode::elevenBit, false);
        readingFormat = ReadingFormat::elevenBitShift;
    }
    else if (type == IpmbType::ADM1278HSC)
    {
        commandAddress = meAddress;
        switch (subType)
        {
            case IpmbSubType::temp:
            case IpmbSubType::curr:
                if (subType == IpmbSubType::temp)
                {
                    commandData = getRawPmbusCommand(
                        ipmi::sensor::smbusMessageTypeReadWord,
                        {static_cast<uint8_t>(
                            ipmi::sensor::PmbusRequest::readTemperature)},
                        ipmi::sensor::PmbusResponseLength::readTemperature,
                        ipmi::sensor::AddressMode::eightBit, true);
                }
                else
                {
                    commandData = getRawPmbusCommand(
                        ipmi::sensor::smbusMessageTypeReadWord,
                        {static_cast<uint8_t>(
                            ipmi::sensor::PmbusRequest::readCurrentOutput)},
                        ipmi::sensor::PmbusResponseLength::readCurrentOutput,
                        ipmi::sensor::AddressMode::eightBit, true);
                }
                netfn = ipmi::me_bridge::netFn;
                command = ipmi::me_bridge::sendRawPmbus;
                readingFormat = ReadingFormat::elevenBit;
                break;
            case IpmbSubType::power:
            case IpmbSubType::volt:
                netfn = ipmi::sensor::netFn;
                command = ipmi::sensor::getSensorReading;
                commandData = {deviceAddress};
                readingFormat = ReadingFormat::byte0;
                break;
            default:
                throw std::runtime_error("Invalid sensor type");
        }
    }
    else if (type == IpmbType::mpsVR)
    {
        commandAddress = meAddress;
        netfn = ipmi::me_bridge::netFn;
        command = ipmi::me_bridge::sendRawPmbus;
        initCommand = ipmi::me_bridge::sendRawPmbus;
        // pmbus read temp
        commandData = getRawPmbusCommand(
            ipmi::sensor::smbusMessageTypeReadWord,
            {static_cast<uint8_t>(ipmi::sensor::PmbusRequest::readTemperature)},
            ipmi::sensor::PmbusResponseLength::readTemperature,
            ipmi::sensor::AddressMode::elevenBit, false);
        // goto page 0
        initData = getRawPmbusCommand(
            ipmi::sensor::smbusMessageTypeWriteByte, {0x00, 0x00},
            ipmi::sensor::PmbusResponseLength::writeByte,
            ipmi::sensor::AddressMode::elevenBit, false);
        readingFormat = ReadingFormat::byte3;
    }
    else
    {
        throw std::runtime_error("Invalid sensor type");
    }

    if (subType == IpmbSubType::util)
    {
        // Utilization need to be scaled to percent
        maxValue = 100;
        minValue = 0;
    }
}

void IpmbSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}

bool IpmbSensor::processReading(const std::vector<uint8_t>& data, double& resp)
{

    switch (readingFormat)
    {
        case (ReadingFormat::byte0):
        {
            if (command == ipmi::sensor::getSensorReading &&
                !ipmi::sensor::isValid(data))
            {
                return false;
            }
            resp = data[0];
            return true;
        }
        case (ReadingFormat::byte3):
        {
            if (data.size() < 4)
            {
                if (errCount == 0U)
                {
                    std::cerr << "Invalid data length returned for " << name
                              << "\n";
                }
                return false;
            }
            resp = data[3];
            return true;
        }
        case (ReadingFormat::elevenBit):
        {
            if (data.size() < 5)
            {
                if (errCount == 0U)
                {
                    std::cerr << "Invalid data length returned for " << name
                              << "\n";
                }
                return false;
            }

            int16_t value = ((data[4] << 8) | data[3]);
            resp = value;
            return true;
        }
        case (ReadingFormat::elevenBitShift):
        {
            if (data.size() < 5)
            {
                if (errCount == 0U)
                {
                    std::cerr << "Invalid data length returned for " << name
                              << "\n";
                }
                return false;
            }

            resp = ((data[4] << 8) | data[3]) >> 3;
            return true;
        }
        case (ReadingFormat::linearElevenBit):
        {
            if (data.size() < 5)
            {
                if (errCount == 0U)
                {
                    std::cerr << "Invalid data length returned for " << name
                              << "\n";
                }
                return false;
            }

            int16_t value = ((data[4] << 8) | data[3]);
            constexpr const size_t shift = 16 - 11; // 11bit into 16bit
            value <<= shift;
            value >>= shift;
            resp = value;
            return true;
        }
        default:
            throw std::runtime_error("Invalid reading type");
    }
}

void IpmbSensor::ipmbRequestCompletionCb(const boost::system::error_code& ec,
                                         const IpmbMethodType& response)
{
    const int& status = std::get<0>(response);
    if (ec || (status != 0))
    {
        incrementError();
        read();
        return;
    }
    const std::vector<uint8_t>& data = std::get<5>(response);
    if constexpr (debug)
    {
        std::cout << name << ": ";
        for (size_t d : data)
        {
            std::cout << d << " ";
        }
        std::cout << "\n";
    }
    if (data.empty())
    {
        incrementError();
        read();
        return;
    }

    double value = 0;

    if (!processReading(data, value))
    {
        incrementError();
        read();
        return;
    }

    // rawValue only used in debug logging
    // up to 5th byte in data are used to derive value
    size_t end = std::min(sizeof(uint64_t), data.size());
    uint64_t rawData = 0;
    for (size_t i = 0; i < end; i++)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<uint8_t*>(&rawData)[i] = data[i];
    }
    rawValue = static_cast<double>(rawData);

    /* Adjust value as per scale and offset */
    value = (value * scaleVal) + offsetVal;
    updateValue(value);
    read();
}

void IpmbSensor::read(void)
{
    waitTimer.expires_from_now(std::chrono::milliseconds(sensorPollMs));
    waitTimer.async_wait(
        [weakRef{weak_from_this()}](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            return; // we're being canceled
        }
        std::shared_ptr<IpmbSensor> self = weakRef.lock();
        if (!self)
        {
            return;
        }
        self->sendIpmbRequest();
    });
}

void IpmbSensor::sendIpmbRequest()
{
    if (!readingStateGood())
    {
        updateValue(std::numeric_limits<double>::quiet_NaN());
        read();
        return;
    }
    dbusConnection->async_method_call(
        [weakRef{weak_from_this()}](boost::system::error_code ec,
                                    const IpmbMethodType& response) {
        std::shared_ptr<IpmbSensor> self = weakRef.lock();
        if (!self)
        {
            return;
        }
        self->ipmbRequestCompletionCb(ec, response);
        },
        "xyz.openbmc_project.Ipmi.Channel.Ipmb",
        "/xyz/openbmc_project/Ipmi/Channel/Ipmb", "org.openbmc.Ipmb",
        "sendRequest", commandAddress, netfn, lun, command, commandData);
}

bool IpmbSensor::sensorClassType(const std::string& sensorClass)
{
    if (sensorClass == "PxeBridgeTemp")
    {
        type = IpmbType::PXE1410CVR;
    }
    else if (sensorClass == "IRBridgeTemp")
    {
        type = IpmbType::IR38363VR;
    }
    else if (sensorClass == "HSCBridge")
    {
        type = IpmbType::ADM1278HSC;
    }
    else if (sensorClass == "MpsBridgeTemp")
    {
        type = IpmbType::mpsVR;
    }
    else if (sensorClass == "METemp" || sensorClass == "MESensor")
    {
        type = IpmbType::meSensor;
    }
    else
    {
        std::cerr << "Invalid class " << sensorClass << "\n";
        return false;
    }
    return true;
}

void IpmbSensor::sensorSubType(const std::string& sensorTypeName)
{
    if (sensorTypeName == "voltage")
    {
        subType = IpmbSubType::volt;
    }
    else if (sensorTypeName == "power")
    {
        subType = IpmbSubType::power;
    }
    else if (sensorTypeName == "current")
    {
        subType = IpmbSubType::curr;
    }
    else if (sensorTypeName == "utilization")
    {
        subType = IpmbSubType::util;
    }
    else
    {
        subType = IpmbSubType::temp;
    }
}

void IpmbSensor::parseConfigValues(const SensorBaseConfigMap& entry)
{
    auto findScaleVal = entry.find("ScaleValue");
    if (findScaleVal != entry.end())
    {
        scaleVal = std::visit(VariantToDoubleVisitor(), findScaleVal->second);
    }

    auto findOffsetVal = entry.find("OffsetValue");
    if (findOffsetVal != entry.end())
    {
        offsetVal = std::visit(VariantToDoubleVisitor(), findOffsetVal->second);
    }

    readState = getPowerState(entry);
}

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    boost::container::flat_map<std::string, std::shared_ptr<IpmbSensor>>&
        sensors,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }
    dbusConnection->async_method_call(
        [&](boost::system::error_code ec, const ManagedObjectType& resp) {
        if (ec)
        {
            std::cerr << "Error contacting entity manager\n";
            return;
        }
        for (const auto& [path, interfaces] : resp)
        {
            for (const auto& [intf, cfg] : interfaces)
            {
                if (intf != configInterfaceName(sensorType))
                {
                    continue;
                }
                std::string name = loadVariant<std::string>(cfg, "Name");

                std::vector<thresholds::Threshold> sensorThresholds;
                if (!parseThresholdsFromConfig(interfaces, sensorThresholds))
                {
                    std::cerr << "error populating thresholds for " << name
                              << "\n";
                }
                uint8_t deviceAddress = loadVariant<uint8_t>(cfg, "Address");

                std::string sensorClass =
                    loadVariant<std::string>(cfg, "Class");

                uint8_t hostSMbusIndex = hostSMbusIndexDefault;
                auto findSmType = cfg.find("HostSMbusIndex");
                if (findSmType != cfg.end())
                {
                    hostSMbusIndex = std::visit(VariantToUnsignedIntVisitor(),
                                                findSmType->second);
                }

                float pollRate = getPollRate(cfg, pollRateDefault);

                uint8_t ipmbBusIndex = ipmbBusIndexDefault;
                auto findBusType = cfg.find("Bus");
                if (findBusType != cfg.end())
                {
                    ipmbBusIndex = std::visit(VariantToUnsignedIntVisitor(),
                                              findBusType->second);
                    std::cerr << "Ipmb Bus Index for " << name << " is "
                              << static_cast<int>(ipmbBusIndex) << "\n";
                }

                /* Default sensor type is "temperature" */
                std::string sensorTypeName = "temperature";
                auto findType = cfg.find("SensorType");
                if (findType != cfg.end())
                {
                    sensorTypeName =
                        std::visit(VariantToStringVisitor(), findType->second);
                }

                auto& sensor = sensors[name];
                sensor = nullptr;
                sensor = std::make_shared<IpmbSensor>(
                    dbusConnection, io, name, path, objectServer,
                    std::move(sensorThresholds), deviceAddress, hostSMbusIndex,
                    pollRate, sensorTypeName);

                sensor->parseConfigValues(cfg);
                if (!(sensor->sensorClassType(sensorClass)))
                {
                    continue;
                }
                sensor->sensorSubType(sensorTypeName);
                sensor->init();
            }
        }
        },
        entityManagerName, "/xyz/openbmc_project/inventory",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

void sdrHandler(sdbusplus::message_t& message,
                std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    std::string objectName;
    SensorBaseConfigMap values;
    message.read(objectName, values);

    auto findBus = values.find("Bus");
    if (findBus == values.end())
    {
        return;
    }

    uint8_t busIndex = loadVariant<uint8_t>(values, "Bus");

    auto& sdrsen = sdrsensor[busIndex];
    sdrsen = nullptr;
    sdrsen = std::make_shared<IpmbSDRDevice>(dbusConnection, busIndex);
    sdrsen->getSDRRepositoryInfo();
}

void reinitSensors(sdbusplus::message_t& message)
{
    constexpr const size_t reinitWaitSeconds = 2;
    std::string objectName;
    boost::container::flat_map<std::string, std::variant<std::string>> values;
    message.read(objectName, values);

    auto findStatus = values.find(power::property);
    if (findStatus != values.end())
    {
        bool powerStatus =
            std::get<std::string>(findStatus->second).ends_with(".Running");
        if (powerStatus)
        {
            if (!initCmdTimer)
            {
                // this should be impossible
                return;
            }
            // we seem to send this command too fast sometimes, wait before
            // sending
            initCmdTimer->expires_from_now(
                std::chrono::seconds(reinitWaitSeconds));

            initCmdTimer->async_wait([](const boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }

                for (const auto& [name, sensor] : sensors)
                {
                    if (sensor)
                    {
                        sensor->runInitCmd();
                    }
                }
            });
        }
    }
}

int main()
{

    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server objectServer(systemBus, true);
    objectServer.add_manager("/xyz/openbmc_project/sensors");
    systemBus->request_name("xyz.openbmc_project.IpmbSensor");

    initCmdTimer = std::make_unique<boost::asio::steady_timer>(io);

    io.post([&]() { createSensors(io, objectServer, sensors, systemBus); });

    boost::asio::steady_timer configTimer(io);

    std::function<void(sdbusplus::message_t&)> eventHandler =
        [&](sdbusplus::message_t&) {
        configTimer.expires_from_now(std::chrono::seconds(1));
        // create a timer because normally multiple properties change
        configTimer.async_wait([&](const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                return; // we're being canceled
            }
            createSensors(io, objectServer, sensors, systemBus);
            if (sensors.empty())
            {
                std::cout << "Configuration not detected\n";
            }
        });
    };

    std::vector<std::unique_ptr<sdbusplus::bus::match_t>> matches =
        setupPropertiesChangedMatches(
            *systemBus, std::to_array<const char*>({sensorType}), eventHandler);

    sdbusplus::bus::match_t powerChangeMatch(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',interface='" + std::string(properties::interface) +
            "',path='" + std::string(power::path) + "',arg0='" +
            std::string(power::interface) + "'",
        reinitSensors);

    auto matchSignal = std::make_shared<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*systemBus),
        "type='signal',member='PropertiesChanged',path_namespace='" +
            std::string(inventoryPath) + "',arg0namespace='" +
            configInterfaceName(sdrInterface) + "'",
        [&systemBus](sdbusplus::message_t& msg) {
        sdrHandler(msg, systemBus);
        });

    setupManufacturingModeMatch(*systemBus);
    io.run();
    return 0;
}
