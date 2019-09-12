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

#include <systemd/sd-journal.h>

#include <ChassisIntrusionSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/asio/sd_event.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/server.hpp>
#include <sdbusplus/timer.hpp>

static constexpr bool DEBUG = false;

static constexpr const char* sensorType =
    "xyz.openbmc_project.Configuration.ChassisIntrusionSensor";
static constexpr const char* nicType = "xyz.openbmc_project.Configuration.NIC";

namespace fs = std::filesystem;

static bool getIntrusionSensorConfig(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    IntrusionSensorType* pType, int* pBusId, int* pSlaveAddr, int* pGpioIndex,
    bool* pGpioInverted)
{
    // find matched configuration according to sensor type
    ManagedObjectType sensorConfigurations;
    bool useCache = false;

    if (!getSensorConfiguration(sensorType, dbusConnection,
                                sensorConfigurations, useCache))
    {
        std::cerr << "error communicating to entity manager\n";
        return false;
    }

    const SensorData* sensorData = nullptr;
    const std::pair<std::string,
                    boost::container::flat_map<std::string, BasicVariantType>>*
        baseConfiguration = nullptr;

    // Get bus and addr of matched configuration
    for (const std::pair<sdbusplus::message::object_path, SensorData>& sensor :
         sensorConfigurations)
    {
        baseConfiguration = nullptr;
        sensorData = &(sensor.second);

        // match sensor type
        auto sensorBase = sensorData->find(sensorType);
        if (sensorBase == sensorData->end())
        {
            std::cerr << "error finding base configuration \n";
            continue;
        }

        baseConfiguration = &(*sensorBase);

        // judge class, "Gpio" or "I2C"
        auto findClass = baseConfiguration->second.find("Class");
        if (findClass != baseConfiguration->second.end() &&
            sdbusplus::message::variant_ns::get<std::string>(
                findClass->second) == "Gpio")
        {
            *pType = IntrusionSensorType::gpio;
        }
        else
        {
            *pType = IntrusionSensorType::pch;
        }

        // case to find GPIO info
        if (*pType == IntrusionSensorType::gpio)
        {
            auto gpioConfig =
                sensorData->find(sensorType + std::string(".GpioIntrusion"));

            if (gpioConfig == sensorData->end())
            {
                std::cerr
                    << "error finding GpioIntrusion info in configuration \n";
                continue;
            }

            auto findGpioIndex = gpioConfig->second.find("Index");
            auto findGpioPolarity = gpioConfig->second.find("Polarity");

            if (findGpioIndex == gpioConfig->second.end() ||
                findGpioPolarity == gpioConfig->second.end())
            {
                std::cerr << "error finding gpio info in configuration \n";
                continue;
            }

            try
            {
                *pGpioIndex = sdbusplus::message::variant_ns::get<uint64_t>(
                    findGpioIndex->second);
                *pGpioInverted =
                    (sdbusplus::message::variant_ns::get<std::string>(
                         findGpioPolarity->second) == "Low");
            }
            catch (const std::bad_variant_access& e)
            {
                std::cerr << "invalid value for gpio info in config. \n";
                continue;
            }

            if (DEBUG)
            {
                std::cout << "find matched GPIO index " << *pGpioIndex
                          << ", polarity inverted flag is " << *pGpioInverted
                          << "\n";
            }

            return true;
        }

        // case to find I2C info
        else if (*pType == IntrusionSensorType::pch)
        {
            auto findBus = baseConfiguration->second.find("Bus");
            auto findAddress = baseConfiguration->second.find("Address");
            if (findBus == baseConfiguration->second.end() ||
                findAddress == baseConfiguration->second.end())
            {
                std::cerr << "error finding bus or address in configuration \n";
                continue;
            }

            try
            {
                *pBusId = sdbusplus::message::variant_ns::get<uint64_t>(
                    findBus->second);
                *pSlaveAddr = sdbusplus::message::variant_ns::get<uint64_t>(
                    findAddress->second);
            }
            catch (const std::bad_variant_access& e)
            {
                std::cerr << "invalid value for bus or address in config. \n";
                continue;
            }

            if (DEBUG)
            {
                std::cout << "find matched bus " << *pBusId
                          << ", matched slave addr " << *pSlaveAddr << "\n";
            }
            return true;
        }
    }

    std::cerr << "can't find matched I2C or GPIO configuration for intrusion "
                 "sensor. \n";
    *pBusId = -1;
    *pSlaveAddr = -1;
    *pGpioIndex = -1;
    return false;
}

static constexpr bool debugLanLeash = false;
boost::container::flat_map<int, bool> lanStatusMap;
boost::container::flat_map<int, std::string> lanInfoMap;
boost::container::flat_map<std::string, int> pathSuffixMap;

using PropertyMapType =
    boost::container::flat_map<std::string, BasicVariantType>;

bool getMatchedConfigurations(
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    std::vector<PropertyMapType>& propMaps, const char* configType)
{
    propMaps.clear();

    // call mapper to get matched obj paths
    GetSubTreeType subtree;
    constexpr int32_t scanDepth = 3;
    const std::array<const char*, 1> configTypes = { configType };
    sdbusplus::message::message method =
        conn->new_method_call("xyz.openbmc_project.ObjectMapper",
                              "/xyz/openbmc_project/object_mapper",
                              "xyz.openbmc_project.ObjectMapper", "GetSubTree");
    method.append("/xyz/openbmc_project/inventory/system", scanDepth,
                  configTypes);

    try
    {
        sdbusplus::message::message reply = conn->call(method);
        reply.read(subtree);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        std::cerr << "Exception happened when communicating to ObjectMapper, "
                  << "error: " << e.what() << "\n";
        return false;
    }

    for (const auto& object : subtree)
    {
        std::string pathName = object.first;
        if (debugLanLeash)
        {
            std::cout << "find matched path: " << pathName << "\n";
        }
        for (const auto& serviceIface : object.second)
        {
            std::string serviceName = serviceIface.first;
            if (debugLanLeash)
            {
                std::cout << " - [service] " << serviceName << "\n";
            }
            for (const auto& interface : serviceIface.second)
            {
                // only get property of matched interface
                bool isIfaceMatched = (configType == interface);
                if (!isIfaceMatched)
                {
                    continue;
                }

                if (debugLanLeash)
                {
                    std::cout << " - [I/F] " << interface << "\n";
                }

                PropertyMapType propMap;
                sdbusplus::message::message method2 = conn->new_method_call(
                    serviceName.c_str(), pathName.c_str(),
                    "org.freedesktop.DBus.Properties", "GetAll");
                method2.append(interface);
                try
                {
                    sdbusplus::message::message reply2 = conn->call(method2);
                    reply2.read(propMap);
                    propMaps.emplace_back(propMap);

                    if (debugLanLeash)
                    {
                        for (auto& item : propMap)
                        {
                            std::cout << "PropertyName: " << item.first << "\n";
                        }
                    }
                }
                catch (const sdbusplus::exception::exception& e)
                {
                    std::cerr << "Exception happened when get all properties,"
                              << " error: " << e.what() << "\n";
                    return false;
                }
            }
        }
    }

    return (propMaps.size() > 0);
}

static void
    getNicNameInfo(std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    // fetch all matched configurations, and parse for required info
    std::vector<PropertyMapType> propMaps;
    bool ret = getMatchedConfigurations(dbusConnection, propMaps, nicType);
    if (!ret)
    {
        std::cerr << "failed to find matched configurations \n";
        return;
    }

    for (PropertyMapType& propMap : propMaps)
    {
        const auto findEthIndex = propMap.find("EthIndex");
        const auto findName = propMap.find("Name");

        if (findEthIndex != propMap.end() && findName != propMap.end())
        {
            auto* pEthIndex = std::get_if<uint64_t>(&findEthIndex->second);
            auto* pName = std::get_if<std::string>(&findName->second);
            if (pEthIndex != nullptr && pName != nullptr)
            {
                lanInfoMap[*pEthIndex] = *pName;
                if (debugLanLeash)
                {
                    std::cout << "find name of eth" << *pEthIndex << " is "
                              << *pName << "\n";
                }
            }
        }
    }

    if (lanInfoMap.size() == 0)
    {
        std::cerr << "can't find matched NIC name. \n";
    }
}

static void processLanStatusChange(sdbusplus::message::message& message)
{
    const std::string& pathName = message.get_path();
    std::string interfaceName;
    boost::container::flat_map<std::string, BasicVariantType> properties;
    message.read(interfaceName, properties);

    auto findStateProperty = properties.find("OperationalState");
    if (findStateProperty == properties.end())
    {
        return;
    }
    std::string* pState =
        std::get_if<std::string>(&(findStateProperty->second));
    if (pState == nullptr)
    {
        std::cerr << "invalid OperationalState \n";
        return;
    }

    bool newLanConnected = (*pState == "routable" || *pState == "carrier" ||
                            *pState == "degraded");

    // get ethNum from path. /org/freedesktop/network1/link/_32 for eth0
    int pos = pathName.find("/_");
    if (pos == std::string::npos || pathName.length() <= pos + 2)
    {
        std::cerr << "unexpected path name " << pathName << "\n";
        return;
    }
    std::string suffixStr = pathName.substr(pos + 2);

    auto findEthNum = pathSuffixMap.find(suffixStr);
    if (findEthNum == pathSuffixMap.end())
    {
        std::cerr << "unexpected eth for suffixStr " << suffixStr << "\n";
        return;
    }
    int ethNum = findEthNum->second;

    // get lan status from map
    auto findLanStatus = lanStatusMap.find(ethNum);
    if (findLanStatus == lanStatusMap.end())
    {
        std::cerr << "unexpected eth " << ethNum << " in lanStatusMap \n";
        return;
    }
    bool oldLanConnected = findLanStatus->second;

    // get lan info from map
    std::string lanInfo = "";
    if (lanInfoMap.size() > 0)
    {
        auto findLanInfo = lanInfoMap.find(ethNum);
        if (findLanInfo == lanInfoMap.end())
        {
            std::cerr << "unexpected eth " << ethNum << " in lanInfoMap \n";
        }
        else
        {
            lanInfo = "(" + findLanInfo->second + ")";
        }
    }

    if (debugLanLeash)
    {
        std::cout << "ethNum = " << ethNum << ", state = " << *pState
                  << ", oldLanConnected = "
                  << (oldLanConnected ? "true" : "false")
                  << ", newLanConnected = "
                  << (newLanConnected ? "true" : "false") << "\n";
    }

    if (oldLanConnected != newLanConnected)
    {
        std::string strEthNum = "eth" + std::to_string(ethNum) + lanInfo;
        std::string strEvent = strEthNum + " LAN leash " +
                               (newLanConnected ? "connected" : "lost");
        std::string strMsgId = "OpenBMC.0.1.PhysicalSecurity";
        sd_journal_send("MESSAGE=%s", strEvent.c_str(), "PRIORITY=%i", LOG_INFO,
                        "REDFISH_MESSAGE_ID=%s", strMsgId.c_str(),
                        "REDFISH_MESSAGE_ARGS=%s", strEvent.c_str(), NULL);
        lanStatusMap[ethNum] = newLanConnected;
        if (debugLanLeash)
        {
            std::cout << "log redfish event: " << strEvent << "\n";
        }
    }
}

static void
    monitorLanStatusChange(std::shared_ptr<sdbusplus::asio::connection> conn)
{
    // init lan port name from configuration
    getNicNameInfo(conn);

    std::vector<fs::path> files;
    if (!findFiles(fs::path("/sys/class/net/"), R"(eth\d+/ifindex)", files))
    {
        std::cerr << "No eth in system\n";
        return;
    }

    // iterate through all found eth files, and save ifindex
    for (auto& fileName : files)
    {
        if (debugLanLeash)
        {
            std::cout << "Reading " << fileName << "\n";
        }
        std::ifstream sysFile(fileName);
        if (!sysFile.good())
        {
            std::cerr << "Failure reading " << fileName << "\n";
            continue;
        }
        std::string line;
        getline(sysFile, line);
        const uint8_t ifindex = std::stoi(line);
        // pathSuffix is ASCII of ifindex
        const std::string& pathSuffix = std::to_string(ifindex + 30);

        // extract ethNum
        const std::string& fileStr = fileName.string();
        const int pos = fileStr.find("eth");
        const std::string& ethNumStr = fileStr.substr(pos + 3);
        int ethNum = 0;
        try
        {
            ethNum = std::stoul(ethNumStr);
        }
        catch (const std::invalid_argument& err)
        {
            std::cerr << "invalid ethNum string: " << ethNumStr << "\n";
            continue;
        }

        // save pathSuffix
        pathSuffixMap[pathSuffix] = ethNum;
        if (debugLanLeash)
        {
            std::cout << "ethNum = " << std::to_string(ethNum)
                      << ", ifindex = " << line
                      << ", pathSuffix = " << pathSuffix << "\n";
        }

        // init lan connected status
        conn->async_method_call(
            [ethNum](boost::system::error_code ec,
                     const std::variant<std::string>& property) {
                if (ec)
                {
                    return;
                }
                const std::string* pState = std::get_if<std::string>(&property);
                if (pState == nullptr)
                {
                    std::cerr << "Unable to read lan status value\n";
                    return;
                }
                bool isLanConnected =
                    (*pState == "routable" || *pState == "carrier" ||
                     *pState == "degraded");
                if (debugLanLeash)
                {
                    std::cout << "ethNum = " << std::to_string(ethNum)
                              << ", init LAN status = "
                              << (isLanConnected ? "true" : "false") << "\n";
                }
                lanStatusMap[ethNum] = isLanConnected;
            },
            "org.freedesktop.network1",
            "/org/freedesktop/network1/link/_" + pathSuffix,
            "org.freedesktop.DBus.Properties", "Get",
            "org.freedesktop.network1.Link", "OperationalState");
    }

    // add match to monitor lan status change
    static sdbusplus::bus::match::match match(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal', member='PropertiesChanged',"
        "arg0namespace='org.freedesktop.network1.Link'",
        [](sdbusplus::message::message& msg) { processLanStatusChange(msg); });

    // add match to monitor entity manager signal about nic name config change
    static sdbusplus::bus::match::match match2(
        static_cast<sdbusplus::bus::bus&>(*conn),
        "type='signal', member='PropertiesChanged',path_namespace='" +
            std::string(inventoryPath) + "',arg0namespace='" + nicType + "'",
        [&conn](sdbusplus::message::message& msg) {
            if (msg.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            getNicNameInfo(conn);
        });
}

int main()
{
    int busId = -1, slaveAddr = -1, gpioIndex = -1;
    bool gpioInverted = false;
    IntrusionSensorType type = IntrusionSensorType::gpio;

    // setup connection to dbus
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    auto objServer = sdbusplus::asio::object_server(systemBus);

    // setup object server, define interface
    systemBus->request_name("xyz.openbmc_project.IntrusionSensor");

    std::shared_ptr<sdbusplus::asio::dbus_interface> ifaceChassis =
        objServer.add_interface(
            "/xyz/openbmc_project/Intrusion/Chassis_Intrusion",
            "xyz.openbmc_project.Chassis.Intrusion");

    ChassisIntrusionSensor chassisIntrusionSensor(io, ifaceChassis);

    if (getIntrusionSensorConfig(systemBus, &type, &busId, &slaveAddr,
                                 &gpioIndex, &gpioInverted))
    {
        chassisIntrusionSensor.start(type, busId, slaveAddr, gpioIndex,
                                     gpioInverted);
    }

    // callback to handle configuration change
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }

            std::cout << "rescan due to configuration change \n";
            if (getIntrusionSensorConfig(systemBus, &type, &busId, &slaveAddr,
                                         &gpioIndex, &gpioInverted))
            {
                chassisIntrusionSensor.start(type, busId, slaveAddr, gpioIndex,
                                             gpioInverted);
            }
        };

    auto match = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*systemBus),
        "type='signal',member='PropertiesChanged',path_namespace='" +
            std::string(inventoryPath) + "',arg0namespace='" + sensorType + "'",
        eventHandler);

    monitorLanStatusChange(systemBus);

    io.run();

    return 0;
}
