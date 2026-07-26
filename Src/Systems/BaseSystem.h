#pragma once

struct ICore;

#include "Services/ServiceRegister.h"
#include <core.hpp>
#include <functional>
#include <utility>
#include <vector>

class BaseSystem
{
  public:
    explicit BaseSystem(ICore &core, const ServiceRegister &serviceRegister)
        : m_core(core), m_serviceRegister(serviceRegister)
    {
    }

    // Отписка от всех диспатчеров, на которые система подписалась через listen().
    // Без неё ядро продолжало бы держать указатели на разрушенные системы: при
    // остановке сервера оно ещё рассылает события (тот же дисконнект всех игроков),
    // и вызов пришёлся бы на уже мёртвый объект.
    virtual ~BaseSystem()
    {
        for (auto it = m_detach.rbegin(); it != m_detach.rend(); ++it)
        {
            (*it)();
        }
    }

    virtual void initialize(IComponentList *components)
    {
    }

    virtual void reset()
    {
    }

  protected:
    // Подписка на диспатчер ядра с автоматической отпиской в деструкторе.
    // Диспатчеры живут в ядре и компонентах, то есть дольше систем (геймод
    // разрушается по free() до выгрузки ядра), поэтому ссылку захватывать безопасно.
    template <typename Dispatcher, typename Handler> void listen(Dispatcher &dispatcher, Handler *handler)
    {
        dispatcher.addEventHandler(handler);
        m_detach.emplace_back(
            [&dispatcher, handler]()
            {
                dispatcher.removeEventHandler(handler);
            });
    }

    ICore &m_core;
    const ServiceRegister &m_serviceRegister;

  private:
    std::vector<std::function<void()>> m_detach;
};
