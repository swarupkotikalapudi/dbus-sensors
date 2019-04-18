#pragma once

#include <sdbusplus/asio/object_server.hpp>

class PSUSubEvent
{
  public:
    PSUSubEvent(std::shared_ptr<sdbusplus::asio::dbus_interface> eventInterface,
                const std::string& path,
                sdbusplus::asio::object_server& objectSever,
                boost::asio::io_service& io, const std::string& eventName,
                std::shared_ptr<int> asserts);
    ~PSUSubEvent();

    std::shared_ptr<sdbusplus::asio::dbus_interface> eventInterface;
    std::shared_ptr<int> assert = std::make_shared<int>(0);

  private:
    int value;
    int errCount;
    std::string path;
    std::string eventName;
    sdbusplus::asio::object_server& objServer;
    boost::asio::deadline_timer waitTimer;
    boost::asio::streambuf readBuf;
    void setupRead(void);
    void handleResponse(const boost::system::error_code& err);
    void updateValue(const int& newValue);
    boost::asio::posix::stream_descriptor inputDev;
    static constexpr unsigned int sensorPollMs = 500;
    static constexpr size_t warnAfterErrorCount = 10;
};

class PSUEvent
{
  public:
    PSUEvent(const std::vector<std::string>& path,
             sdbusplus::asio::object_server& objectServer,
             boost::asio::io_service& io, const std::string& psuName,
             const std::string& eventName);
    ~PSUEvent();

    std::shared_ptr<sdbusplus::asio::dbus_interface> eventInterface;
    std::string name;

  private:
    std::vector<std::string> paths;
    sdbusplus::asio::object_server& objServer;
    std::string eventName;
    boost::container::flat_map<int, std::unique_ptr<PSUSubEvent>> subEvents;
};
