#pragma once

#include "Macro.h"
#include "Services/Core/NavigationLockService/NavigationLockService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <string>
#include <vector>

// GPS-навигация (бизнес-фича, НЕ Core):
//  * /gps (всем) — диалог-каталог мест + пункт «Отключить GPS». Выбор места ставит
//    красный чекпоинт-указатель до точки через VehicleWaypointService (единый
//    владелец обычного чекпоинт-слота: «последний выигрывает» с парковкой/домом);
//    вход в чекпоинт гасит маркер и шлёт сообщение о прибытии;
//  * /tp (админ 1+) — тот же каталог, но выбор СЕРВЕРНО телепортирует админа через
//    PlayerLocationService (grace анти-чита; не сырой setPosition).
//
// Каталог мест — единая таблица (m_places): статические точки мирового контента +
// динамические «Мой дом» (вход своего дома) и «Моя организация» (база своей фракции),
// резолвятся от игрока в момент клика (образец SpawnChoiceSystem). Пункты видны
// всем всегда; недоступность (нет дома/не член орга) объясняет сообщение при клике,
// не скрытие (конвенция UI проекта).
//
// Лок навигации (NavigationLockService): пока держится (в смене работы), /gps
// отказывает выбором места с причиной; диалог всё равно открывается (пункты видны
// всегда). Взятие лока гасит активный GPS-маркер (подписка subscribeAcquired).
// На /tp лок НЕ распространяется — телепорт чекпоинт-слот не трогает.
class GpsSystem : public BaseSystem
{
  public:
    GpsSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Именованное место каталога. Static — фиксированные координаты мира; Home/Work —
    // динамические, координаты резолвятся от игрока при клике (position/interior/vw в
    // самой записи не используются).
    struct Place
    {
        enum class Kind
        {
            Static,
            Home,
            Work
        };

        const char *name; // utf-8
        Kind kind;
        Vector3 position;
        unsigned interior;
        int virtualWorld;
    };

    // Резолв места: позиция/интерьер/мир от игрока в момент клика.
    struct ResolvedPlace
    {
        Vector3 position{};
        unsigned interior = 0;
        int virtualWorld = 0;
    };

    // --- команды ---
    void showGpsDialog(IPlayer &player);
    void showTpDialog(IPlayer &player);

    // --- действия по выбору ---
    void selectGpsPlace(IPlayer &player, int index);
    void disableGps(IPlayer &player);
    void teleportToPlace(IPlayer &player, int index);

    // Тело LIST из имён мест (общее для /gps и /tp), utf-8, порядок = m_places.
    std::string buildPlacesBody() const;
    // Резолв места index от игрока; false + сообщение игроку, если место недоступно
    // (нет дома / не член орга). Общий для /gps и /tp. navigation=true (GPS) уводит
    // «Мою организацию» на УЛИЧНЫЙ вход базы (достижимо чекпоинтом); false (телепорт)
    // — на интерьерный spawn базы (телепорт переносит интерьер/мир).
    bool resolvePlace(IPlayer &player, int index, bool navigation, ResolvedPlace &out);

    NavigationLockService &m_navLockService;
    VehicleWaypointService &m_waypointService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerSessionService &m_sessionService;
    HouseService &m_houseService;
    FactionService &m_factionService;
    PlayerDialogService &m_dialogService;

    std::vector<Place> m_places; // единый каталог (легко правимый), заполняется в конструкторе
};
