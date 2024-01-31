#pragma once

#include "MCTPEndpoint.hpp"

class MCTPDeviceRepository
{
  private:
    // FIXME: Ugh, hack. Figure out a better data structure?
    std::map<std::string, std::shared_ptr<MCTPDevice>> devices;

    auto lookup(const std::shared_ptr<MCTPDevice>& device)
    {
        auto pred = [&device](const auto& it) { return it.second == device; };
        return std::ranges::find_if(devices, pred);
    }

  public:
    MCTPDeviceRepository() = default;
    MCTPDeviceRepository(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository(MCTPDeviceRepository&&) = delete;
    ~MCTPDeviceRepository() = default;

    MCTPDeviceRepository& operator=(const MCTPDeviceRepository&) = delete;
    MCTPDeviceRepository& operator=(MCTPDeviceRepository&&) = delete;

    void add(const std::string& inventory,
             const std::shared_ptr<MCTPDevice>& device)
    {
        auto [_, fresh] = devices.emplace(inventory, device);
        if (!fresh)
        {
            throw std::logic_error(
                std::format("Tried to add entry for existing device: {}",
                            device->describe()));
        }
    }

    void remove(const std::shared_ptr<MCTPDevice>& device)
    {
        auto entry = lookup(device);
        if (entry == devices.end())
        {
            throw std::logic_error(
                std::format("Trying to remove unknown device: {}",
                            entry->second->describe()));
        }
        devices.erase(entry);
    }

    void remove(const std::string& inventory)
    {
        auto entry = devices.find(inventory);
        if (entry == devices.end())
        {
            throw std::logic_error(std::format(
                "Trying to remove unknown inventory: {}", inventory));
        }
        devices.erase(entry);
    }

    bool contains(const std::string& inventory)
    {
        return devices.contains(inventory);
    }

    bool contains(const std::shared_ptr<MCTPDevice>& device)
    {
        return lookup(device) != devices.end();
    }

    const std::string& inventoryFor(const std::shared_ptr<MCTPDevice>& device)
    {
        auto entry = lookup(device);
        if (entry == devices.end())
        {
            throw std::logic_error(
                std::format("Cannot retrieve inventory for unknown device: {}",
                            device->describe()));
        }
        return entry->first;
    }

    const std::shared_ptr<MCTPDevice>& deviceFor(const std::string& inventory)
    {
        auto entry = devices.find(inventory);
        if (entry == devices.end())
        {
            throw std::logic_error(std::format(
                "Cannot retrieve device for unknown inventory: {}", inventory));
        }
        return entry->second;
    }
};
