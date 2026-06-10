#pragma once

#include "Services/IService.h"
#include <assert.h>
#include <cassert>
#include <memory>
#include <typeindex>
#include <unordered_map>

class ServiceRegister final
{
  public:
    void registerServices();

    template <typename T> T &getService() const
    {
        static_assert(std::is_base_of<IService, T>::value, "T must derive from IService");

        auto it = m_services.find(std::type_index(typeid(T)));
        if (it != m_services.end())
        {
            return *static_cast<T *>(it->second.get());
        }

        assert(false && "Service not found");
        return *static_cast<T *>(
            nullptr); // This line will never be reached, but it prevents compiler warnings about no return value.
    }

  private:
    template <typename T> void registerService()
    {
        static_assert(std::is_base_of<IService, T>::value, "T must derive from IService");
        m_services[std::type_index(typeid(T))] = std::make_unique<T>();
    }

    std::unordered_map<std::type_index, std::unique_ptr<IService>> m_services;
};
