#include <sdk.hpp>

#include "ThreadPool/ThreadPool.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/ServiceRegister.h"
#include "Systems/SystemRegister.h"
#include <Server/Components/Dialogs/dialogs.hpp>

class GameMode : public IComponent, public CoreEventHandler
{
  public:
    StringView componentName() const override
    {
        return "GameMode";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(1, 0, 0, BUILD_NUMBER);
    }

    void onLoad(ICore *core) override
    {
        LogManager::initialize(core);
        ThreadPool::initialize(std::thread::hardware_concurrency() - 1);
        DatabaseManager::initialize();

        m_core = core;
        core->getEventDispatcher().addEventHandler(this);

        m_serviceRegister.registerServices();
        m_systemRegister.registerSystems(*core, m_serviceRegister);

        core->useStuntBonuses(false);
    }

    void onInit(IComponentList *components) override
    {
        m_systemRegister.initializeSystems(components);
    }

    void free() override
    {
        // Без shutdown статический вектор joinable-потоков разрушился бы при
        // выгрузке компонента → std::terminate. Заодно дорабатывается очередь
        // задач (записи в БД) и выполняются их колбэки.
        ThreadPool::shutdown();

        // Ядро переживает выгрузку геймода: оставленный в его диспатчере указатель
        // на нас — вызов onTick по освобождённой памяти.
        if (m_core)
            m_core->getEventDispatcher().removeEventHandler(this);

        delete this;
    };

    void reset() override
    {
        m_systemRegister.resetSystems();
    }

    UID getUID() override
    {
        return 0;
    }

    void onTick(Microseconds elapsed, TimePoint now) override
    {
        ThreadPool::flush();
    }

  private:
    ICore *m_core = nullptr;
    ServiceRegister m_serviceRegister;
    SystemRegister m_systemRegister;
};

COMPONENT_ENTRY_POINT()
{
    return new GameMode();
}
