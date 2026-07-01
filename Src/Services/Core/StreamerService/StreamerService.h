#pragma once

#include "Macro.h"
#include "Services/Core/GridService/GridService.h"
#include "Services/IService.h"
#include "player.hpp"
#include <Server/Components/Pickups/pickups.hpp>
#include <array>
#include <cstdint>
#include <unordered_map>
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
//    периодическая развёртка создаёт/удаляет). open.mp сканирует ВЕСЬ активный
//    пул на стрим-тик каждого игрока, поэтому пул держим минимальным: «нужен»
//    ставится только игроком в virtual world пикапа и в пределах его радиуса.
//
// Ограничение: у объектов, иконок и лейблов НЕТ virtual world — глобальный
// визуал стримера виден из всех миров на тех же координатах (vw есть только у
// пикапов). Класть контент в интерьеры через стример нельзя без расширения defs.
//
// Производительность: проход игрока — один обход сетки с фильтром по радиусу
// стрима каждого def'а на месте, отбор ближайших в бюджет (nth_element — только
// при переполнении) и линейный merge-diff двух сортированных массивов
// (показанное vs желаемое); создания/удаления — только по фактической разнице.
// Выход из зоны — с гистерезисом (1.2 радиуса), чтобы движение вдоль границы не
// дёргало release/create. Все буферы переиспользуются, на установившемся режиме
// проход не аллоцирует.
class StreamerService final : public IService
{
  public:
    static constexpr float MAX_STREAM_DISTANCE = 300.0f; // и радиус запроса к сетке

    // Бюджеты на игрока. Клиентский лимит объектов ~1000 (включая глобальные),
    // иконок — 100; берём с запасом под ручное использование.
    static constexpr int OBJECT_BUDGET = 400;
    static constexpr int ICON_BUDGET = 90;   // слоты 0..89, слоты 90..99 свободны для ручных иконок
    static constexpr int LABEL_BUDGET = 200; // per-player пул лейблов — 1024; запас под ручные/прикреплённые

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
    // position/virtualWorld — принятые из PlayerLocationService (не сырые клиентские).
    void streamPlayer(IPlayer &player, const Vector3 &position, int virtualWorld,
                      TimePoint now);   // внутри троттлится сам
    void sweepPickups(TimePoint now);   // глобальная развёртка пикапов
    void resetPlayer(int playerId);

    // Def id пикапа по id в пуле (для маршрутизации onPlayerPickUpPickup), -1 —
    // пикап не из стримера. O(1) по обратному индексу: клиент шлёт RPC подбора
    // повторно каждый кадр, пока стоит на пикапе, — резолв на каждом событии.
    int pickupDefByPoolId(int poolId) const;
    // Позиция и мир def'а пикапа (для валидации подбора).
    bool getPickupInfo(int defId, Vector3 &position, std::uint32_t &virtualWorld) const;

    // --- отладка/инспекция ---
    std::size_t shownObjectCount(int playerId) const
    {
        if (playerId < 0 || playerId >= MAX_PLAYERS)
            return 0;
        return m_players[playerId].objects.size();
    }
    std::size_t shownIconCount(int playerId) const
    {
        if (playerId < 0 || playerId >= MAX_PLAYERS)
            return 0;
        return m_players[playerId].icons.size();
    }
    std::size_t shownLabelCount(int playerId) const
    {
        if (playerId < 0 || playerId >= MAX_PLAYERS)
            return 0;
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

    // Кандидат прохода: def, прошедший фильтр радиуса, до отбора в бюджет.
    struct Candidate
    {
        int defId;
        float distSq;
    };

    void diffObjects(IPlayer &player, PerPlayer &pp);
    void diffIcons(IPlayer &player, PerPlayer &pp);
    void diffLabels(IPlayer &player, PerPlayer &pp);

    // defId сейчас показан? Бинарный поиск по shown (сортирован по defId), O(log S).
    static bool isShown(const std::vector<Shown> &shown, int defId);
    // Отбирает в desired не больше budget БЛИЖАЙШИХ кандидатов (candidates при
    // переполнении усекается на месте; порядок внутри бюджета не важен).
    static void selectDesired(std::vector<Candidate> &candidates, int budget, std::vector<int> &desired);

    ICore *m_core = nullptr;
    GridService *m_grid = nullptr;
    IPickupsComponent *m_pickups = nullptr;
    int m_activePickups = 0; // сколько пикапов сейчас живёт в пуле

    std::vector<ObjectDef> m_objectDefs;
    std::vector<int> m_freeObjectDefs;
    std::vector<PickupDef> m_pickupDefs;
    std::vector<int> m_freePickupDefs;
    // Обратный индекс poolId -> defId. Зеркалит ровно ЖИВЫЕ пул-экземпляры
    // (def.poolId != -1): вставка при create, стирание при release/removePickup.
    // Пул open.mp динамический, poolId не ограничен константой — только map.
    std::unordered_map<int, int> m_pickupPoolToDef;
    std::vector<IconDef> m_iconDefs;
    std::vector<int> m_freeIconDefs;
    std::vector<LabelDef> m_labelDefs;
    std::vector<int> m_freeLabelDefs;

    std::array<PerPlayer, MAX_PLAYERS> m_players;

    // Переиспользуемые буферы прохода (один поток, не реентерабельно).
    std::vector<Candidate> m_candObjects;
    std::vector<Candidate> m_candIcons;
    std::vector<Candidate> m_candLabels;
    std::vector<int> m_desiredObjects;
    std::vector<int> m_desiredIcons;
    std::vector<int> m_desiredLabels;
    std::vector<Shown> m_scratchShown;
};
