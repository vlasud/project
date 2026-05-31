#include <memory>
#include <player.hpp>
#include <sdk.hpp>

#include "Systems/ISystem.h"
#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"
#include "Systems/SpawnSystem/SpawnSystem.h"

#include "ThreadPool/ThreadPool.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"

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
        ThreadPool::initialize(std::thread::hardware_concurrency());
        DatabaseManager::initialize();

        core->getEventDispatcher().addEventHandler(this);

        m_systems.push_back(std::make_unique<PlayerAuthSystem>());
        m_systems.push_back(std::make_unique<SpawnSystem>());

        for (const auto &system : m_systems)
        {
            system->link(core);
        }
    }

    void onInit(IComponentList *components) override
    {
        for (const auto &system : m_systems)
        {
            system->initialize();
        }
    }

    void free() override
    {
        delete this;
    };

    void reset() override
    {
        for (auto &system : m_systems)
        {
            system->reset();
        }
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
    std::vector<std::unique_ptr<ISystem>> m_systems;
};

COMPONENT_ENTRY_POINT()
{
    return new GameMode();
}
