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

#include "filesystem.hpp"

#include <PSUSensor.hpp>
#include <Utils.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/container/flat_set.hpp>
#include <fstream>
#include <regex>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

static constexpr std::array<const char*, 1> sensorTypes = {
    "xyz.openbmc_project.Configuration.pmbus"};

namespace fs = std::filesystem;

static bool checkSensorMax(std::string sensorPathStr, double& value,
                           unsigned int& factor)
{
    if (factor == 0)
    {
        std::cerr << "check max failed, factor is 0\n";
        return false;
    }
    auto maxPath = boost::replace_all_copy(sensorPathStr, "input", "max");
    std::ifstream maxFile(maxPath);
    if (!maxFile.good())
    {
        return false; // no _max file for this sensor in sysfs
    }
    std::string max;
    std::getline(maxFile, max);
    maxFile.close();
    value = std::stod(max) / factor;
    return true;
}

static bool checkSensorMin(std::string sensorPathStr, double& value,
                           unsigned int& factor)
{
    if (factor == 0)
    {
        std::cerr << "check min failed, factor is 0\n";
        return false;
    }
    auto minPath = boost::replace_all_copy(sensorPathStr, "input", "min");
    std::ifstream minFile(minPath);
    if (!minFile.good())
    {
        return false; // no _min file for this sensor in sysfs
    }
    std::string min;
    std::getline(minFile, min);
    minFile.close();
    value = std::stod(min) / factor;
    return true;
}

void createSensors(
    boost::asio::io_service& io, sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    boost::container::flat_map<std::string, std::unique_ptr<PSUSensor>>&
        sensors,
    boost::container::flat_map<std::string, std::unique_ptr<PSUProperty>>&
        sensorTable,
    boost::container::flat_map<std::string, std::string>& labelMatch)
{

    ManagedObjectType sensorConfigs;
    bool useCache = false;

    for (const char* type : sensorTypes)
    {
        if (!getSensorConfiguration(type, dbusConnection, sensorConfigs,
                                    useCache))
        {
            std::cerr << "error get sensor config from entity manager\n";
            return;
        }
        useCache = true;
    }

    std::vector<fs::path> pmbusPaths;
    if (!findFiles(fs::path("/sys/class/hwmon"), "name", pmbusPaths))
    {
        std::cerr << "No PSU sensors in system\n";
        return;
    }

    boost::container::flat_set<std::string> directories;
    for (const auto& pmbusPath : pmbusPaths)
    {
        const std::string pathStr = pmbusPath.string();
        auto directory = pmbusPath.parent_path();

        auto ret = directories.insert(directory.string());
        if (!ret.second)
        {
            continue; // check if path i1 already searched
        }

        auto device = fs::path(directory / "device");
        std::string deviceName = fs::canonical(device).stem();
        auto findHyphen = deviceName.find("-");
        if (findHyphen == std::string::npos)
        {
            std::cerr << "found bad device" << deviceName << "\n";
            continue;
        }
        std::string busStr = deviceName.substr(0, findHyphen);
        std::string addrStr = deviceName.substr(findHyphen + 1);

        size_t bus = 0;
        size_t addr = 0;

        try
        {
            bus = std::stoi(busStr);
            addr = std::stoi(addrStr, 0, 16);
        }
        catch (std::invalid_argument)
        {
            continue;
        }

        std::ifstream nameFile(pmbusPath);
        if (!nameFile.good())
        {
            std::cerr << "Failure reading " << pmbusPath << "\n";
            continue;
        }

        std::string pmbusName;
        std::getline(nameFile, pmbusName);
        nameFile.close();
        if (pmbusName != "pmbus")
        {
            continue;
        }

        const std::pair<std::string, boost::container::flat_map<
                                         std::string, BasicVariantType>>*
            baseConfig = nullptr;
        const SensorData* sensorData = nullptr;
        const std::string* interfacePath = nullptr;
        const char* sensorType = nullptr;

        for (const std::pair<sdbusplus::message::object_path, SensorData>&
                 sensor : sensorConfigs)
        {
            sensorData = &(sensor.second);
            for (const char* type : sensorTypes)
            {
                auto sensorBase = sensorData->find(type);
                if (sensorBase != sensorData->end())
                {
                    baseConfig = &(*sensorBase);
                    sensorType = type;
                    break;
                }
            }
            if (baseConfig == nullptr)
            {
                std::cerr << "error finding base configuration for "
                          << deviceName << "\n";
                continue;
            }

            auto configBus = baseConfig->second.find("Bus");
            auto configAddress = baseConfig->second.find("Address");

            if (configBus == baseConfig->second.end() ||
                configAddress == baseConfig->second.end())
            {
                std::cerr << "error finding necessary entry in configuration";
                continue;
            }

            if (std::get<uint64_t>(configBus->second) != bus ||
                std::get<uint64_t>(configAddress->second) != addr)
            {
                continue;
            }

            interfacePath = &(sensor.first.str);
            break;
        }
        if (interfacePath == nullptr)
        {
            std::cerr << "failed to find match for " << deviceName << "\n";
            continue;
        }

        auto findSensorName = baseConfig->second.find("Name");
        if (findSensorName == baseConfig->second.end())
        {
            std::cerr << "could not determine configuration name for "
                      << deviceName << "\n";
            continue;
        }

        std::vector<fs::path> sensorPaths;
        if (!findFiles(fs::path(directory), R"(\w\d+_input$)", sensorPaths, 0))
        {
            std::cerr << "No PSU non-label sensor in PSU\n";
            continue;
        }

        for (const auto& sensorPath : sensorPaths)
        {

            std::string labelHead;
            std::string sensorPathStr = sensorPath.string();
            std::string sensorNameStr = sensorPath.filename();
            std::string sensorNameSubStr =
                sensorNameStr.substr(0, sensorNameStr.find("_") - 1);

            std::string labelPathStr =
                boost::replace_all_copy(sensorNameStr, "input", "label");
            std::vector<fs::path> labelPaths;
            if (!findFiles(fs::path(directory), labelPathStr, labelPaths, 0))
            {
                std::cerr << "No PSU non-label sensor in PSU\n";
                continue;
            }

            if (labelPaths.empty())
            {
                labelHead = sensorNameStr.substr(0, sensorNameStr.find("_"));
            }
            else
            {
                auto labelPath =
                    boost::replace_all_copy(sensorPathStr, "input", "label");
                std::ifstream labelFile(labelPath);
                if (!labelFile.good())
                {
                    std::cerr << "Failure reading " << sensorPath << "\n";
                    continue;
                }
                std::string label;
                std::getline(labelFile, label);
                labelFile.close();

                auto findSensor = sensors.find(label);
                if (findSensor != sensors.end())
                {
                    continue;
                }

                labelHead = label.substr(0, label.find(" "));
            }

            std::vector<thresholds::Threshold> sensorThresholds;

            parseThresholdsFromConfig(*sensorData, sensorThresholds,
                                      &labelHead);

            auto findProperty = sensorTable.find(sensorNameSubStr);
            if (findProperty == sensorTable.end())
            {
                std::cerr << "Cannot find PSU sensorType\n";
                continue;
            }

            if (sensorThresholds.empty())
            {
                if (!parseThresholdsFromAttr(
                        sensorThresholds, sensorPathStr,
                        findProperty->second->sensorScaleFactor))
                {
                    std::cerr << "error populating thresholds\n";
                }
            }

            std::string labelName;
            auto findLabel = labelMatch.find(labelHead);
            if (findLabel != labelMatch.end())
            {
                labelName = findLabel->second;
            }
            else
            {
                labelName = labelHead;
            }

            double maxReading = 0;
            double minReading = 0;
            if (!checkSensorMax(sensorPathStr, maxReading,
                                findProperty->second->sensorScaleFactor))
            {
                maxReading = findProperty->second->maxReading;
            }
            if (!checkSensorMin(sensorPathStr, minReading,
                                findProperty->second->sensorScaleFactor))
            {
                minReading = findProperty->second->minReading;
            }

            std::string sensorName =
                std::get<std::string>(findSensorName->second) + " " + labelName;

            sensors[sensorName] = std::make_unique<PSUSensor>(
                sensorPathStr, sensorType, objectServer, dbusConnection, io,
                sensorName, std::move(sensorThresholds), *interfacePath,
                findProperty->second->sensorTypeName,
                findProperty->second->sensorScaleFactor, maxReading,
                minReading);
        }
    }
    return;
}

void propertyInitialize(
    boost::container::flat_map<std::string, std::unique_ptr<PSUProperty>>&
        sensorTable,
    boost::container::flat_map<std::string, std::string>& labelMatch)
{
    sensorTable["power"] =
        std::make_unique<PSUProperty>("power/", 3000, 0, 100000);
    sensorTable["curr"] =
        std::make_unique<PSUProperty>("current/", 255, 0, 1000);
    sensorTable["temp"] =
        std::make_unique<PSUProperty>("temperature/", 127, -128, 1000);
    sensorTable["in"] = std::make_unique<PSUProperty>("voltage/", 255, 0, 1000);
    sensorTable["fan"] = std::make_unique<PSUProperty>("fan/", 65535, 0, 1);

    boost::container::flat_map<std::string, std::string> labelList{
        {"pin", "Input Power"},
        {"pout1", "Output Power 1"},
        {"pout2", "Output Power 2"},
        {"vin", "Input Voltage"},
        {"vout1", "Output Voltage 1"},
        {"vout2", "Output Voltage 2"},
        {"iin", "Input Current"},
        {"iout1", "Output Current 1"},
        {"iout2", "Output Current 2"},
        {"temp1", "Inlet Temperature 1"},
        {"temp2", "Inlet Temperature 2"},
        {"fan1", "Fan 1"},
        {"fan2", "Fan 2"}};

    labelMatch.swap(labelList);
}

int main(int argc, char** argv)
{
    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);

    systemBus->request_name("xyz.openbmc_project.PSUSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    boost::container::flat_map<std::string, std::unique_ptr<PSUSensor>> sensors;
    boost::container::flat_map<std::string, std::unique_ptr<PSUProperty>>
        sensorTable;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;
    boost::container::flat_map<std::string, std::string> labelMatch;

    propertyInitialize(sensorTable, labelMatch);

    io.post([&]() {
        createSensors(io, objectServer, systemBus, sensors, sensorTable,
                      labelMatch);
    });
    boost::asio::deadline_timer filterTimer(io);
    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            if (message.is_method_error())
            {
                std::cerr << "callback method error\n";
                return;
            }
            filterTimer.expires_from_now(boost::posix_time::seconds(1));
            filterTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return;
                }
                else if (ec)
                {
                    std::cerr << "timer error\n";
                }
                createSensors(io, objectServer, systemBus, sensors, sensorTable,
                              labelMatch);
            });
        };

    for (const char* type : sensorTypes)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }
    io.run();
}
