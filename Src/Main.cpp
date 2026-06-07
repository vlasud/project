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
    ServiceRegister m_serviceRegister;
    SystemRegister m_systemRegister;
};

COMPONENT_ENTRY_POINT()
{
    return new GameMode();
}
