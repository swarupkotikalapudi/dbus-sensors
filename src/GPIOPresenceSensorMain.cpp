#include <GPIOPresenceSensor.hpp>
#include <Utils.hpp>

#include <iostream>
#include <Utils.hpp>
#include <sdbusplus/bus/match.hpp>

namespace gpiopresencesensing
{
std::unique_ptr<sdbusplus::bus::match::match> ifcAdded;
std::unique_ptr<sdbusplus::bus::match::match> ifcRemoved;

using OnInterfaceAddedCallback =
    std::function<void(std::string_view, std::string_view, const Config&)>;
using OnInterfaceRemovedCallback = std::function<void(std::string_view)>;

// Helper function to convert dbus property to struct
// @param[in] properties: dbus properties
Config getConfig(const SensorBaseConfigMap& properties, std::string parentPath)
{
    auto name = loadVariant<std::string>(properties, Properties::propertyName);
    auto gpioLine =
        loadVariant<std::string>(properties, Properties::propertyGpioLine);
    auto polarity =
        loadVariant<std::string>(properties, Properties::propertyPolarity);

    // Optional variables
    int pollRate = pollRateDefault;
    auto propertyPollRate = properties.find(Properties::propertyPollRate);
    if (propertyPollRate != properties.end())
    {
        pollRate =
            loadVariant<uint32_t>(properties, Properties::propertyPollRate);
    }

    // Optional Association
    bool generateAssociation = false;
    std::string associationForward = "";
    std::string associationReverse = "";
    std::string associationPath = "";

    auto propertyAssociationPath =
        properties.find(Properties::propertyAssociationPath);
    if (propertyAssociationPath != properties.end())
    {
        associationPath =
            loadVariant<std::string>(properties,
                Properties::propertyAssociationPath);
        
        auto propertyAssociationForward =
            properties.find(Properties::propertyAssociationForward);
        if (propertyAssociationForward != properties.end())
        {
            associationForward =
                loadVariant<std::string>(properties,
                    Properties::propertyAssociationForward);

            auto propertyAssociationReverse =
                properties.find(Properties::propertyAssociationReverse);\
            if (propertyAssociationReverse != properties.end())
            {
                associationReverse =
                    loadVariant<std::string>(properties,
                        Properties::propertyAssociationReverse);
                generateAssociation = true;
            }
        }
    }

    return {name, gpioLine, polarity == "active_low",
            /*present*/ false, pollRate, generateAssociation, associationPath,
            associationForward, associationReverse, parentPath};
}

void setupInterfaceAdded(sdbusplus::asio::connection* conn,
                         OnInterfaceAddedCallback&& cb)
{
    std::function<void(sdbusplus::message::message & msg)> handler =
        [callback = cb](sdbusplus::message::message& msg) {
            sdbusplus::message::object_path objPath;
            SensorData ifcAndProperties;
            msg.read(objPath, ifcAndProperties);
            auto found =
                ifcAndProperties.find(interfaces::emGPIOCableSensingIfc);
            if (found != ifcAndProperties.end())
            {
                Config config;
                try
                {
                    config = getConfig(found->second, objPath.parent_path());
                    callback(objPath.str, found->first, config);
                }
                catch (std::exception& e)
                {
                    std::cerr << "Incomplete config found: " << e.what()
                              << " obj = " << objPath.str << std::endl;
                }
            }
        };

    // call the user callback for all the device that is already available
    conn->async_method_call(
        [callback = cb](const boost::system::error_code ec,
             ManagedObjectType managedObjs) {
            if (ec)
            {
                return;
            }
            for (auto& obj : managedObjs)
            {
                SensorData& item = obj.second;

                auto found = item.find(interfaces::emGPIOCableSensingIfc);
                if (found != item.end())
                {
                    Config config;
                    try
                    {
                        config = getConfig(found->second,
                            obj.first.parent_path());
                        callback(obj.first.str, found->first, config);
                    }
                    catch (std::exception& e)
                    {
                        std::cerr << "Incomplete config found: " << e.what()
                                  << " obj = " << obj.first.str << std::endl;
                    }
                }
            }
        },
        "xyz.openbmc_project.EntityManager", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

    ifcAdded = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        sdbusplus::bus::match::rules::interfacesAdded() +
            sdbusplus::bus::match::rules::sender(
                "xyz.openbmc_project.EntityManager"),
        handler);
}

void setupInterfaceRemoved(sdbusplus::asio::connection* conn,
                           OnInterfaceRemovedCallback&& cb)
{
    // Listen to the interface removed event.
    std::function<void(sdbusplus::message::message&)> handler =
        [callback = std::move(cb)](sdbusplus::message::message msg) {
            sdbusplus::message::object_path objPath;
            msg.read(objPath);
            callback(objPath.str);
        };
    ifcRemoved = std::make_unique<sdbusplus::bus::match::match>(
        static_cast<sdbusplus::bus::bus&>(*conn),
        sdbusplus::bus::match::rules::interfacesRemoved() +
            sdbusplus::bus::match::rules::sender(
                "xyz.openbmc_project.EntityManager"),
        handler);
}
} // namespace gpiopresencesensing

void startMain(
    int delay, const std::shared_ptr<sdbusplus::asio::connection>& bus,
    const std::shared_ptr<gpiopresencesensing::GPIOPresence>& controller)
{
    static boost::asio::steady_timer timer(bus->get_io_context());
    timer.cancel();
    timer.expires_after(std::chrono::seconds(delay));
    timer.async_wait(
        [delay, bus, controller](const boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                std::cout << "Delaying update loop" << std::endl;
                return;
            }
            std::cout << "Update loop started" << std::endl;
            controller->startUpdateLoop(/*forceUpdate=*/true);
        });
}

int main()
{
    boost::asio::io_context io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name(gpiopresencesensing::service);
    sdbusplus::asio::object_server objectServer(systemBus);

    std::shared_ptr<gpiopresencesensing::GPIOPresence> controller =
        std::make_shared<gpiopresencesensing::GPIOPresence>(systemBus);

    // Wait for config from EM.
    // Once a new config is posted. We will create the obj on dbus.
    gpiopresencesensing::setupInterfaceAdded(
        systemBus.get(), [&controller, &systemBus, &objectServer](
                             std::string_view, std::string_view,
                             const gpiopresencesensing::Config& config) {
            sdbusplus::message::object_path inventoryPath(
                gpiopresencesensing::inventoryObjPath);
            sdbusplus::message::object_path objPath =
                inventoryPath / config.name;
            
            std::cout << "New config received " << objPath.str << std::endl;

            if (!controller->hasObj(objPath.str))
            {
                controller->removeObj(objPath.str);
            }
            // Add Status
            auto statusIfc = objectServer.add_unique_interface(
                objPath, gpiopresencesensing::interfaces::statusIfc);
            std::cout << "Adding status interface: " << config.name
                << " at path: " << objPath.str << std::endl;
            statusIfc->register_property(
                gpiopresencesensing::Properties::propertyPresent, false);
            statusIfc->register_property("Name", config.name);
            statusIfc->initialize();
            // Add Inventory Association
            if (config.generateAssociation) {
                sdbusplus::message::object_path associationPath(
                    config.associationPath);

                std::cout << "Adding association interface: "
                    << config.associationPath << " at path: "
                    << associationPath.str << std::endl;

                auto assocIfc = objectServer.add_unique_interface(
                    associationPath, association::interface);
                assocIfc->register_property(
                    "Associations",
                    std::vector<Association>{
                        {
                            config.associationForward,
                            config.associationReverse,
                            config.parentPath
                        }
                    }
                );
                assocIfc->initialize();
                controller->addObj(std::move(statusIfc),
                    std::move(assocIfc), objPath.str, config);
            } else {
                controller->addObj(std::move(statusIfc),
                    nullptr, objPath.str, config);
            }
            controller->setMinPollRate(config.pollRate);
            // It is possible there are more EM config in the pipeline.
            // Therefore, delay the main loop by 10 seconds to wait for more
            // configs.
            // TODO(linchuyuan@google.com): figure out why sometimes there is
            // lag when EM pushes new configs to the gpio daemon.
            startMain(/*delay=*/10, systemBus, controller);
        });

    gpiopresencesensing::setupInterfaceRemoved(
        systemBus.get(), [&controller](std::string_view objPath) {
            controller->removeObj(std::string(objPath));
        });

    io.run();
}
