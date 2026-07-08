#pragma once

#include "Log/LogManager.h"
#include "Services/IService.h"
#include <assert.h>
#include <cassert>
#include <cstdlib>
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

        // Сервис не зарегистрирован — ошибка инициализации, продолжать нельзя ни в debug, ни в release.
        assert(false && "Service not found");
        LogManager::log(LogLevel::Error, std::string("ServiceRegister: service not found: ") + typeid(T).name());
        std::abort(); // безусловное падение — assert в release compiles out, разыменование nullptr ниже было бы UB
    }

  private:
    template <typename T> void registerService()
    {
        static_assert(std::is_base_of<IService, T>::value, "T must derive from IService");
        m_services[std::type_index(typeid(T))] = std::make_unique<T>();
    }

    std::unordered_map<std::type_index, std::unique_ptr<IService>> m_services;
};
