#pragma once

#include "Macro.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/IService.h"
#include "player.hpp"
#include <Server/Components/Pickups/pickups.hpp>
#include <array>
#include <cstdint>
#include <vector>

// Стример: позволяет создавать неограниченное число объектов, пикапов и иконок
// карты поверх клиентских лимитов. Хранит определения (defs), а реальные
// клиентские сущности создаёт/удаляет динамически по близости игроков, используя
// GridService для поиска кандидатов.
//
// Механика по типам:
//  * объекты  — per-player (IPlayerObjectData): каждому игроку показываются
//    ближайшие OBJECT_BUDGET объектов в радиусе стрима;
//  * иконки   — per-player слоты setMapIcon (клиентский лимит 100; используем
//    слоты 0..ICON_BUDGET-1, остальные свободны для ручного использования);
//  * пикапы   — пул глобальный, поэтому стриминг глобальный: пикап существует в
//    пуле, пока хотя бы один игрок рядом (проходы игроков помечают «нужен»,
//    периодическая развёртка создаёт/удаляет).
//
// Производительность: проход игрока — один запрос к сетке, одна сортировка
// кандидатов по дистанции и линейный merge-diff двух сортированных массивов
// (показанное vs желаемое); создания/удаления — только по фактической разнице.
// Все буферы переиспользуются, на установившемся режиме проход не аллоцирует.
class StreamerService final : public IService
{
  public:
    static constexpr float MAX_STREAM_DISTANCE = 300.0f; // и радиус запроса к сетке

    // Вызывается StreamerSystem::initialize до любых add*.
    void initialize(ICore &core, GridService &grid, IPickupsComponent *pickups);

    // --- контент (возвращают def id; -1 при ошибке) ---
    int addObject(int model, const Vector3 &position, const Vector3 &rotation,
                  float streamDistance = MAX_STREAM_DISTANCE, float drawDistance = 0.0f);
    void removeObject(int defId);

    int addPickup(int model, PickupType type, const Vector3 &position, std::uint32_t virtualWorld = 0,
                  float streamDistance = MAX_STREAM_DISTANCE);
    void removePickup(int defId);

    int addMapIcon(int iconType, const Vector3 &position, Colour colour,
                   MapIconStyle style = MapIconStyle_Local, float streamDistance = MAX_STREAM_DISTANCE);
    void removeMapIcon(int defId);

    int addTextLabel(StringView text, Colour colour, const Vector3 &position, float drawDistance, bool testLOS = false,
                     float streamDistance = MAX_STREAM_DISTANCE);
    void removeTextLabel(int defId);
    // Живое обновление текста/цвета у всех, кому лейбл сейчас показан.
    bool updateTextLabel(int defId, StringView text, Colour colour);

    // --- вызывается StreamerSystem ---
    // position — принятая позиция из PlayerLocationService (не сырая клиентская).
    void streamPlayer(IPlayer &player, const Vector3 &position, TimePoint now); // внутри троттлится сам
    void sweepPickups(TimePoint now);                  // глобальная развёртка пикапов
    void resetPlayer(int playerId);

    // Def id пикапа по id в пуле (для маршрутизации onPlayerPickUpPickup), -1 —
    // пикап не из стримера. Линейный по числу def'ов — события подбора редкие.
    int pickupDefByPoolId(int poolId) const;
    // Позиция и мир def'а пикапа (для валидации подбора).
    bool getPickupInfo(int defId, Vector3 &position, std::uint32_t &virtualWorld) const;

    // --- отладка/инспекция ---
    std::size_t shownObjectCount(int playerId) const
    {
        return m_players[playerId].objects.size();
    }
    std::size_t shownIconCount(int playerId) const
    {
        return m_players[playerId].icons.size();
    }
    std::size_t shownLabelCount(int playerId) const
    {
        return m_players[playerId].labels.size();
    }
    int activePickupCount() const
    {
        return m_activePickups;
    }

  private:
    struct ObjectDef
    {
        bool used = false;
        int model = 0;
        Vector3 position{};
        Vector3 rotation{};
        float drawDistance = 0.0f;
        float streamDistSq = 0.0f;
        GridService::Handle gridHandle = GridService::INVALID_HANDLE;
    };

    struct PickupDef
    {
        bool used = false;
        int model = 0;
        PickupType type = 0; // PickupType = uint8_t в SDK (тип поведения пикапа)
        std::uint32_t virtualWorld = 0;
        Vector3 position{};
        float streamDistSq = 0.0f;
        GridService::Handle gridHandle = GridService::INVALID_HANDLE;
        int poolId = -1;      // id в пуле пикапов, -1 — не создан
        TimePoint lastWanted; // когда последний раз был нужен кому-то рядом
    };

    struct LabelDef
    {
        bool used = false;
        std::string text; // уже в cp1251
        Colour colour = Colour::White();
        float drawDistance = 30.0f;
        bool testLOS = false;
        Vector3 position{};
        float streamDistSq = 0.0f;
        GridService::Handle gridHandle = GridService::INVALID_HANDLE;
    };

    struct IconDef
    {
        bool used = false;
        int iconType = 0;
        Colour colour = Colour::White();
        MapIconStyle style = MapIconStyle_Local;
        Vector3 position{};
        float streamDistSq = 0.0f;
        GridService::Handle gridHandle = GridService::INVALID_HANDLE;
    };

    struct Shown
    {
        int defId;    // ссылка на def
        int clientId; // id per-player объекта или слот иконки
    };

    struct PerPlayer
    {
        TimePoint nextStreamAt;
        std::vector<Shown> objects; // отсортированы по defId (для merge-diff)
        std::vector<Shown> icons;   // отсортированы по defId
        std::vector<Shown> labels;  // отсортированы по defId
        std::vector<int> freeIconSlots;
        bool iconSlotsInit = false;
    };

    void diffObjects(IPlayer &player, PerPlayer &pp);
    void diffIcons(IPlayer &player, PerPlayer &pp);
    void diffLabels(IPlayer &player, PerPlayer &pp);

    ICore *m_core = nullptr;
    GridService *m_grid = nullptr;
    IPickupsComponent *m_pickups = nullptr;
    int m_activePickups = 0; // сколько пикапов сейчас живёт в пуле

    std::vector<ObjectDef> m_objectDefs;
    std::vector<int> m_freeObjectDefs;
    std::vector<PickupDef> m_pickupDefs;
    std::vector<int> m_freePickupDefs;
    std::vector<IconDef> m_iconDefs;
    std::vector<int> m_freeIconDefs;
    std::vector<LabelDef> m_labelDefs;
    std::vector<int> m_freeLabelDefs;

    std::array<PerPlayer, MAX_PLAYERS> m_players;

    // Переиспользуемые буферы прохода (один поток, не реентерабельно).
    std::vector<GridService::Result> m_candidates;
    std::vector<int> m_desiredObjects;
    std::vector<int> m_desiredIcons;
    std::vector<int> m_desiredLabels;
    std::vector<Shown> m_scratchShown;
};
