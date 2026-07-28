#include "Systems/GpsSystem/GpsSystem.h"

#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <string>

namespace
{
// Игровые цвета сообщений (как в остальных бизнес-системах).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
} // namespace

GpsSystem::GpsSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_navLockService(serviceRegister.getService<NavigationLockService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_houseService(serviceRegister.getService<HouseService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    // Единый каталог мест (легко правимый — одна таблица). Статические координаты —
    // реального мирового контента; динамические резолвятся от игрока при клике.
    m_places = {
        {"Порт — работа грузчиком", Place::Kind::Static, {2746.6726f, -2450.3950f, 13.6484f}, 0, 0},
        {"Автобусное депо — работа водителем", Place::Kind::Static, {1204.0439f, -1823.2920f, 13.5918f}, 0, 0},
        {"Депо развозчиков — работа развозчиком", Place::Kind::Static, {2171.0681f, -2252.5254f, 13.3026f}, 0, 0},
        {"Больница — работа врачом", Place::Kind::Static, {1183.6260f, -1332.1185f, 13.5814f}, 0, 0},
        {"Центральная парковка", Place::Kind::Static, {1626.2488f, -1136.7443f, 23.9063f}, 0, 0},
        {"Банк (Сан-Фиерро)", Place::Kind::Static, {-2766.5515f, 375.5889f, 6.3347f}, 0, 0},
        {"Мэрия (Лос-Сантос)", Place::Kind::Static, {1480.94f, -1772.07f, 18.80f}, 0, 0},
        {"Мой дом", Place::Kind::Home, {}, 0, 0},
        {"Моя организация", Place::Kind::Work, {}, 0, 0},
    };

    // Взятие лока навигации гасит активный GPS-маркер игрока (иначе завис бы поверх
    // рабочих маркеров). Гасим только GPS-цель (clearGpsFor) — парковочный/домашний
    // указатель не трогаем. Игрок валиден (лок берут в игре, при устройстве на работу).
    m_navLockService.subscribeAcquired(
        [this](int playerId)
        {
            if (IPlayer *player = m_core.getPlayers().get(playerId))
            {
                m_waypointService.clearGpsFor(*player);
            }
        });

    // Сброс лока на дисконнекте (переиспользуемый слот не наследует чужой лок).
    // Работы освобождают лок сами на конце смены; это страховочный сброс. GPS-маркер
    // на дисконнекте гасит VehicleWaypointSystem (сброс его слота).
    m_sessionService.subscribeEnd([this](IPlayer &player, const PlayerSessionService::Session &)
                                  { m_navLockService.reset(player.getID()); });

    auto &commands = serviceRegister.getService<PlayerCommandService>();
    // /gps — всем (без гейта прав). Универсальный навигатор качества жизни — в /help,
    // секция «Прочее» (Misc): без строки в справке новичок о нём не узнает.
    commands.add("gps", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showGpsDialog(player); },
                 {}, "проложить маршрут к нужному месту", PlayerCommandService::HelpCategory::Misc);

    // /tp — админ 1+ (эффективный уровень после /alogin, как прочие админ-команды).
    commands.add("tp", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showTpDialog(player); },
                 PermissionSpec::admin(1), "телепорт к выбранному месту каталога",
                 PlayerCommandService::HelpCategory::Hidden);
}

// ------------------------------------------------------------------ /gps

void GpsSystem::showGpsDialog(IPlayer &player)
{
    // Каталог + пункт «Отключить GPS» последней строкой (индекс = размеру каталога).
    // Амбер {FFB400} выделяет управляющее действие среди белых имён мест; цвет-код
    // не сдвигает listItem (индекс = размеру каталога).
    std::string body = buildPlacesBody();
    body += "\n{FFB400}Отключить GPS";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "GPS-навигация", body, "Выбрать", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            const int placeCount = static_cast<int>(m_places.size());
            if (listItem == placeCount) // «Отключить GPS»
            {
                disableGps(*player);
                return;
            }
            if (listItem < 0 || listItem >= placeCount)
            {
                return; // вне диапазона
            }
            selectGpsPlace(*player, listItem);
        });
}

void GpsSystem::selectGpsPlace(IPlayer &player, int index)
{
    const int playerId = player.getID();

    // Лок навигации: выбор места отказывает с причиной (диалог всё равно открылся —
    // конвенция «пункты видны всегда, гейт в обработчике»). Проверка на клике
    // (серверный факт), а не на открытии — лок мог смениться, пока диалог висел.
    if (m_navLockService.isLocked(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("GPS недоступен: {}", m_navLockService.lockReason(playerId))));
        return;
    }

    ResolvedPlace place;
    if (!resolvePlace(player, index, /*navigation=*/true, place))
    {
        return; // resolvePlace уже объяснил недоступность игроку
    }

    // Единый чекпоинт-слот через VehicleWaypointService: новый выбор заменяет прежний
    // GPS-маркер и любой парковочный указатель («последний выигрывает»). Вход в
    // чекпоинт гасит маркер сам и шлёт сообщение о прибытии.
    const std::string name = m_places[index].name;
    m_waypointService.showGpsFor(player, place.position,
                                 [name](IPlayer &p)
                                 {
                                     p.sendClientMessage(INFO_COLOUR,
                                                         u(fmt::format("Вы прибыли: {}. GPS отключён", name)));
                                 });
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("GPS: маршрут до «{}» построен — следуйте за красным чекпоинтом", name)));
}

void GpsSystem::disableGps(IPlayer &player)
{
    if (m_waypointService.hasGpsWaypoint(player.getID()))
    {
        m_waypointService.clearFor(player);
        player.sendClientMessage(INFO_COLOUR, u("GPS отключён"));
    }
    else
    {
        player.sendClientMessage(INFO_COLOUR, u("Активного GPS-маршрута нет"));
    }
}

// ------------------------------------------------------------------ /tp (админ 1+)

void GpsSystem::showTpDialog(IPlayer &player)
{
    // Тот же каталог, что и /gps (переиспользуем построение), без пункта «Отключить GPS».
    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Телепорт", buildPlacesBody(), "Выбрать", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(m_places.size()))
            {
                return;
            }
            teleportToPlace(*player, listItem);
        });
}

void GpsSystem::teleportToPlace(IPlayer &player, int index)
{
    const int playerId = player.getID();

    // Админ за рулём/пассажиром: серверный телепорт двигает только игрока (машина
    // осталась бы позади), поэтому просим выйти из транспорта. Клиенту не верим —
    // стейт серверный.
    const PlayerState state = m_stateService.getState(playerId);
    if (state == PlayerState_Driver || state == PlayerState_Passenger)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы телепортироваться"));
        return;
    }
    if (state != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Сейчас нельзя телепортироваться"));
        return; // мёртв/спектатор/загрузка — позиция не применится осмысленно
    }

    ResolvedPlace place;
    if (!resolvePlace(player, index, /*navigation=*/false, place))
    {
        return;
    }

    // Серверный телепорт с grace анти-чита (интерьер/мир разом) — НЕ сырой setPosition.
    m_locationService.teleport(player, place.position, place.interior, place.virtualWorld);
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы телепортированы: {}", m_places[index].name)));
}

// ------------------------------------------------------------------ helpers

std::string GpsSystem::buildPlacesBody() const
{
    std::string body;
    for (std::size_t i = 0; i < m_places.size(); ++i)
    {
        if (i != 0)
        {
            body += "\n";
        }
        body += m_places[i].name;
    }
    return body;
}

bool GpsSystem::resolvePlace(IPlayer &player, int index, bool navigation, ResolvedPlace &out)
{
    if (index < 0 || index >= static_cast<int>(m_places.size()))
    {
        return false; // защитно — вне диапазона
    }
    const Place &place = m_places[index];
    switch (place.kind)
    {
    case Place::Kind::Static:
        out.position = place.position;
        out.interior = place.interior;
        out.virtualWorld = place.virtualWorld;
        return true;

    case Place::Kind::Home:
    {
        // «Мой дом» — вход своего дома (образец SpawnChoiceSystem): владение по
        // серверному accountId из сессии, не по клиентскому предположению.
        const PlayerSessionService::Session *session = m_sessionService.get(player.getID());
        if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
        {
            player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы использовать это место"));
            return false;
        }
        const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
        if (!house)
        {
            player.sendClientMessage(ERROR_COLOUR, u("У вас нет своего дома. Займите свободный дом на карте"));
            return false;
        }
        out.position = house->entrance; // вход дома (основной мир, интерьер 0)
        out.interior = 0;
        out.virtualWorld = 0;
        return true;
    }

    case Place::Kind::Work:
    {
        // «Моя организация» — база своей фракции (членство по серверным фактам).
        // Не член -> недоступно.
        const int factionId = m_factionService.getMemberFaction(player.getID());
        const FactionService::Faction *faction = m_factionService.getFaction(factionId);
        if (factionId == FactionService::NO_FACTION || !faction || !faction->spawn.defined)
        {
            player.sendClientMessage(ERROR_COLOUR, u("Вы не состоите в организации"));
            return false;
        }
        // Навигация ведёт на УЛИЧНЫЙ вход базы (мир 0): spawn большинства фракций стоит
        // внутри интерьера в приватном мире (vw = id фракции) — чекпоинт до такой точки
        // недостижим (цель висит на ~1000 м, onEnter не сработает). Телепорт переносит
        // интерьер/мир разом, поэтому идёт на сам spawn. Орг без интерьер-базы (Army) —
        // фолбэк на уличный spawn и для навигации.
        if (navigation && faction->base.defined && !faction->base.entrances.empty())
        {
            out.position = faction->base.entrances.front().pickupPos; // улица, мир 0
            out.interior = 0;
            out.virtualWorld = 0;
            return true;
        }
        out.position = faction->spawn.position;
        out.interior = static_cast<unsigned>(faction->spawn.interior);
        out.virtualWorld = faction->spawn.virtualWorld;
        return true;
    }
    }
    return false;
}
