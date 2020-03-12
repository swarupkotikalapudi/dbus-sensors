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

#include "PSUEvent.hpp"

#include <systemd/sd-journal.h>

#include <boost/container/flat_map.hpp>
#include <iostream>
#include <memory>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

PSUCombineEvent::PSUCombineEvent(
    sdbusplus::asio::object_server& objectServer,
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_service& io, const std::string& psuName,
    boost::container::flat_map<std::string, std::vector<std::string>>&
        eventPathList,
    boost::container::flat_map<
        std::string,
        boost::container::flat_map<std::string, std::vector<std::string>>>&
        groupEventPathList,
    const std::string& combineEventName) :
    objServer(objectServer)
{
    eventInterface = objServer.add_interface(
        "/xyz/openbmc_project/State/Decorator/" + psuName + "_" +
            combineEventName,
        "xyz.openbmc_project.State.Decorator.OperationalStatus");
    eventInterface->register_property("functional", true);

    if (!eventInterface->initialize())
    {
        std::cerr << "error initializing event interface\n";
    }

    std::shared_ptr<std::set<std::string>> combineEvent =
        std::make_shared<std::set<std::string>>();
    for (const auto& pathList : eventPathList)
    {
        std::shared_ptr<std::set<std::string>> assert =
            std::make_shared<std::set<std::string>>();
        std::shared_ptr<bool> state = std::make_shared<bool>(false);

        const std::string& eventName = pathList.first;
        std::string eventPSUName = eventName + psuName;
        for (const auto& path : pathList.second)
        {
            events[eventPSUName].emplace_back(std::make_unique<PSUSubEvent>(
                eventInterface, path, conn, io, eventName, eventName, assert,
                combineEvent, state, psuName));
            asserts.emplace_back(assert);
            states.emplace_back(state);
        }
    }

    for (const auto& groupPathList : groupEventPathList)
    {
        for (const auto& pathList : groupPathList.second)
        {
            std::shared_ptr<std::set<std::string>> assert =
                std::make_shared<std::set<std::string>>();
            std::shared_ptr<bool> state = std::make_shared<bool>(false);

            const std::string& groupEventName = pathList.first;
            std::string eventPSUName = groupEventName + psuName;
            for (const auto& path : pathList.second)
            {
                events[eventPSUName].emplace_back(std::make_unique<PSUSubEvent>(
                    eventInterface, path, conn, io, groupEventName,
                    groupPathList.first, assert, combineEvent, state, psuName));
                asserts.emplace_back(assert);
                states.emplace_back(state);
            }
        }
    }
}

PSUCombineEvent::~PSUCombineEvent()
{
    // Clear unique_ptr first
    for (auto& event : events)
    {
        for (auto& subEventPtr : event.second)
        {
            subEventPtr.reset(nullptr);
        }
    }
    events.clear();
    objServer.remove_interface(eventInterface);
}

static boost::container::flat_map<std::string,
                                  std::pair<std::string, std::string>>
    logID = {
        {"PredictiveFailure",
         {"OpenBMC.0.1.PowerSupplyFailurePredicted",
          "OpenBMC.0.1.PowerSupplyPredictedFailureRecovered"}},
        {"Failure",
         {"OpenBMC.0.1.PowerSupplyFailed", "OpenBMC.0.1.PowerSupplyRecovered"}},
        {"ACLost",
         {"OpenBMC.0.1.PowerSupplyPowerLost",
          "OpenBMC.0.1.PowerSupplyPowerRestored"}},
        {"FanFault",
         {"OpenBMC.0.1.PowerSupplyFanFailed",
          "OpenBMC.0.1.PowerSupplyFanRecovered"}},
        {"ConfigureError",
         {"OpenBMC.0.1.PowerSupplyConfigurationError",
          "OpenBMC.0.1.PowerSupplyConfigurationErrorRecovered"}}};

PSUSubEvent::PSUSubEvent(
    std::shared_ptr<sdbusplus::asio::dbus_interface> eventInterface,
    const std::string& path, std::shared_ptr<sdbusplus::asio::connection>& conn,
    boost::asio::io_service& io, const std::string& groupEventName,
    const std::string& eventName,
    std::shared_ptr<std::set<std::string>> asserts,
    std::shared_ptr<std::set<std::string>> combineEvent,
    std::shared_ptr<bool> state, const std::string& psuName) :
    eventInterface(eventInterface),
    asserts(asserts), combineEvent(combineEvent), assertState(state),
    errCount(0), path(path), eventName(eventName), waitTimer(io), inputDev(io),
    psuName(psuName), groupEventName(groupEventName), systemBus(conn)
{
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        std::cerr << "PSU sub event failed to open file\n";
        return;
    }
    inputDev.assign(fd);
    inputDev.non_blocking(true);

    auto found = logID.find(eventName);
    if (found == logID.end())
    {
        assertMessage.clear();
        deassertMessage.clear();
    }
    else
    {
        assertMessage = found->second.first;
        deassertMessage = found->second.second;
    }

    auto fanPos = path.find("fan");
    if (fanPos != std::string::npos)
    {
        fanName = path.substr(fanPos);
        auto fanNamePos = fanName.find("_");
        if (fanNamePos != std::string::npos)
        {
            fanName = fanName.substr(0, fanNamePos);
        }
    }
    setupRead(boost::system::error_code());
}

void PSUSubEvent::setupRead(const boost::system::error_code& err)
{
    if (err != boost::system::errc::success)
    {
        std::cerr << "Sensor event timer " << path
                  << " error: " << err.message() << "\n";
    }

    pendingTime = false;

    // This is a cancellation point, after timer, before reading
    if (deleteQuiescent)
    {
        std::cerr << "Event anomaly: Setup called but already quiescent\n";
    }
    if (deleteRequested)
    {
        deleteQuiescent = true;
        return;
    }

    pendingRead = true;

    boost::asio::async_read_until(
        inputDev, readBuf, '\n',
        [&](const boost::system::error_code& ec,
            std::size_t /*bytes_transfered*/) { handleResponse(ec); });
}

PSUSubEvent::~PSUSubEvent()
{
    // Do not call waitTimer.cancel() here, timer should already be idle,
    // as caller must wait until deleteQuiescent before destructing this.
    inputDev.close();
}

void PSUSubEvent::handleResponse(const boost::system::error_code& err)
{
    if (err.value() != 0)
    {
        std::cerr << "Sensor event response " << path
                  << " error: " << err.message() << "\n";
    }

    pendingRead = false;

    // This is a cancellation point, after reading, before timer
    if (deleteQuiescent)
    {
        std::cerr << "Event anomaly: Response called but already quiescent\n";
    }
    if (deleteRequested)
    {
        deleteQuiescent = true;
        return;
    }

    // Continue processing, except for these two fatal errors,
    // because otherwise, the timer would not be rescheduled.
    if (err == boost::asio::error::operation_aborted)
    {
        return;
    }
    if (err == boost::system::errc::bad_file_descriptor)
    {
        return;
    }

    std::istream responseStream(&readBuf);
    if (!err)
    {
        std::string response;
        try
        {
            std::getline(responseStream, response);
            int nvalue = std::stoi(response);
            responseStream.clear();

            updateValue(nvalue);
            errCount = 0;
        }
        catch (const std::invalid_argument&)
        {
            errCount++;
        }
    }
    else
    {
        errCount++;
    }
    if (errCount >= warnAfterErrorCount)
    {
        if (errCount == warnAfterErrorCount)
        {
            std::cerr << "Failure to read event at " << path << "\n";
        }
        updateValue(0);
        errCount++;
    }

    pendingTime = true;

    lseek(fd, 0, SEEK_SET);
    waitTimer.expires_from_now(boost::posix_time::milliseconds(eventPollMs));
    waitTimer.async_wait(
        [&](const boost::system::error_code& ec) { setupRead(ec); });
}

// Any of the sub events of one event is asserted, then the event will be
// asserted. Only if none of the sub events are asserted, the event will be
// deasserted.
void PSUSubEvent::updateValue(const int& newValue)
{
    // Take no action if value already equal
    // Same semantics as Sensor::updateValue(const double&)
    if (newValue == value)
    {
        return;
    }

    if (newValue == 0)
    {
        // log deassert only after all asserts are gone
        if (!(*asserts).empty())
        {
            auto found = (*asserts).find(path);
            if (found == (*asserts).end())
            {
                return;
            }
            (*asserts).erase(path);

            return;
        }
        if (*assertState == true)
        {
            *assertState = false;
            auto foundCombine = (*combineEvent).find(groupEventName);
            if (foundCombine == (*combineEvent).end())
            {
                return;
            }
            (*combineEvent).erase(groupEventName);
            if (!deassertMessage.empty())
            {
                // Fan Failed has two args
                std::string sendMessage = eventName + " deassert";
                if (deassertMessage == "OpenBMC.0.1.PowerSupplyFanRecovered")
                {
                    sd_journal_send(
                        "MESSAGE=%s", sendMessage.c_str(), "PRIORITY=%i",
                        LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                        deassertMessage.c_str(), "REDFISH_MESSAGE_ARGS=%s,%s",
                        psuName.c_str(), fanName.c_str(), NULL);
                }
                else
                {
                    sd_journal_send(
                        "MESSAGE=%s", sendMessage.c_str(), "PRIORITY=%i",
                        LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                        deassertMessage.c_str(), "REDFISH_MESSAGE_ARGS=%s",
                        psuName.c_str(), NULL);
                }
            }

            if ((*combineEvent).empty())
            {
                eventInterface->set_property("functional", true);
            }
        }
    }
    else
    {
        if ((*assertState == false) && ((*asserts).empty()))
        {
            *assertState = true;
            if (!assertMessage.empty())
            {
                // For failure and configure error, spec requires a beep
                if ((assertMessage == "OpenBMC.0.1.PowerSupplyFailed") ||
                    (assertMessage ==
                     "OpenBMC.0.1.PowerSupplyConfigurationError"))
                {
                    std::cout << " beep for " << assertMessage << "\n";
                    beep(beepPSUFailure);
                }

                // Fan Failed has two args
                std::string sendMessage = eventName + " assert";
                if (assertMessage == "OpenBMC.0.1.PowerSupplyFanFailed")
                {
                    sd_journal_send(
                        "MESSAGE=%s", sendMessage.c_str(), "PRIORITY=%i",
                        LOG_WARNING, "REDFISH_MESSAGE_ID=%s",
                        assertMessage.c_str(), "REDFISH_MESSAGE_ARGS=%s,%s",
                        psuName.c_str(), fanName.c_str(), NULL);
                }
                else
                {
                    sd_journal_send(
                        "MESSAGE=%s", sendMessage.c_str(), "PRIORITY=%i",
                        LOG_WARNING, "REDFISH_MESSAGE_ID=%s",
                        assertMessage.c_str(), "REDFISH_MESSAGE_ARGS=%s",
                        psuName.c_str(), NULL);
                }
            }
            if ((*combineEvent).empty())
            {
                eventInterface->set_property("functional", false);
            }
            (*combineEvent).emplace(groupEventName);
        }
        (*asserts).emplace(path);
    }
    value = newValue;
}

void PSUSubEvent::beep(const uint8_t& beepPriority)
{
    systemBus->async_method_call(
        [](boost::system::error_code ec) {
            if (ec)
            {
                std::cerr << "beep error (ec = " << ec << ")\n";
                return;
            }
        },
        "xyz.openbmc_project.BeepCode", "/xyz/openbmc_project/BeepCode",
        "xyz.openbmc_project.BeepCode", "Beep", uint8_t(beepPriority));
}

bool PSUSubEvent::isDeleteQuiescent() const
{
    return deleteQuiescent;
}

void PSUSubEvent::requestDelete()
{
    deleteRequested = true;

    // If this event is currently idle, it can stay idle
    if (!(pendingTime || pendingRead))
    {
        deleteQuiescent = true;
    }
}

bool PSUCombineEvent::isDeleteQuiescent() const
{
    for (const auto& eventPair : events)
    {
        for (const auto& event : eventPair.second)
        {
            // If one not quiescent, entire object not quiescent
            if (!(event->isDeleteQuiescent()))
            {
                return false;
            }
        }
    }

    return true;
}

void PSUCombineEvent::requestDelete()
{
    for (const auto& eventPair : events)
    {
        for (const auto& event : eventPair.second)
        {
            event->requestDelete();
        }
    }
}
