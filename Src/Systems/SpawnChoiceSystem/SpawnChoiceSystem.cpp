#include "Systems/SpawnChoiceSystem/SpawnChoiceSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Фиксированная точка спавна в ПОРТУ (Лос-Сантос) — дефолт И фолбэк. Координаты
// совпадают с проектным дефолтом (PlayerSpawnService::SpawnPoint), новой константы
// не плодим. Имена STATION_*/Choice::Station оставлены как внутренний id
// «дефолтный выбор» (enum и БД не трогаем); игроку эта точка называется «Порт».
const Vector3 STATION_POSITION{2690.5237f, -2479.6560f, 13.6509f};
constexpr float STATION_ANGLE = 89.2331f;

// Тело LIST: первая ЖёЛТАЯ строка — невыбираемая подсказка (смысл «настройка, не
// телепорт»), под ней три реальных пункта. Поэтому listItem смещён на единицу:
// 0 — подсказка, 1 — вокзал, 2 — дом, 3 — работа (см. dispatch по индексу ниже).
constexpr int HINT_ROWS = 1;

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

SpawnChoiceSystem::SpawnChoiceSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_choiceService(serviceRegister.getService<SpawnChoiceService>()),
      m_spawnService(serviceRegister.getService<PlayerSpawnService>()),
      m_factionService(serviceRegister.getService<FactionService>()),
      m_houseService(serviceRegister.getService<HouseService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadChoice(player, session); });
    m_sessionService.subscribeEnd([this](IPlayer &player, const PlayerSessionService::Session &)
                                  { m_choiceService.reset(player.getID()); });

    // Перерезолв точки спавна при смене членства. (1) На логине членство и выбор
    // грузятся ДВУМЯ async-колбэками; member-change (из FactionService::
    // handleSessionStart) триггерит applySpawn, и какой бы из двух колбэков ни
    // завершился ВТОРЫМ, у него уже есть И членство, И выбор -> последний
    // applySpawn корректен, гонки нет. (2) Член с выбором Work, ВЫШЕДШИЙ из орга в
    // середине сессии, иначе остался бы со спавном в базе (возможно приватный
    // vw/интерьер фракции) — перерезолв даёт фолбэк на порт (дефолт). Игрока берём из
    // аргумента (он гарантированно валиден в момент события).
    m_factionService.subscribeMemberChange([this](IPlayer &player, int, int) { applySpawn(player); });

    // Старт-гонка спавна «Дом»: на логине выбор Home применяется (applySpawn ->
    // resolveSpawn -> houseOf) ДО прихода house_owner из БД -> houseOf ещё не видит
    // владение -> фолбэк на порт (дефолт). По завершении загрузки владения перерезолвим
    // спавн уже-онлайн игрокам с выбором Home (зеркало FamilySystem::loadAll).
    // One-shot: подписка отрабатывает один раз; новые логины после загрузки уже
    // видят владение, повторно дёргать не нужно.
    m_houseService.subscribeOwnershipLoaded(
        [this]
        {
            for (IPlayer *player : m_core.getPlayers().entries())
            {
                if (m_choiceService.getChoice(player->getID()) == SpawnChoiceService::Choice::Home)
                    applySpawn(*player);
            }
        });

    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("setspawn", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showDialog(player); }, {},
                 "выбрать точку появления: вокзал, дом или работа", PlayerCommandService::HelpCategory::Misc);
}

// ------------------------------------------------------------------ загрузка по сессии

void SpawnChoiceSystem::loadChoice(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<int>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult result = schema.getTable("player_spawn")
                                           .select("choice")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            // Нет строки -> Station(0) по умолчанию.
            int choice = static_cast<int>(SpawnChoiceService::Choice::Station);
            if (mysqlx::Row row = result.fetchOne())
                choice = row.get(0).get<int>();
            return choice;
        },
        [this, playerId = player.getID(), serial = session.serial](int choiceValue)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;

            // Нормализация значения БД: 0->Station, 1->Home, 2->Work, прочее->Station.
            SpawnChoiceService::Choice choice = SpawnChoiceService::Choice::Station;
            if (choiceValue == static_cast<int>(SpawnChoiceService::Choice::Home))
                choice = SpawnChoiceService::Choice::Home;
            else if (choiceValue == static_cast<int>(SpawnChoiceService::Choice::Work))
                choice = SpawnChoiceService::Choice::Work;

            m_choiceService.load(playerId, choice);
            // Применяем сразу, чтобы авторизационный спавн пришёл в выбранную точку.
            // Гонки с загрузкой членства нет: member-change перерезолвит спавн, и
            // последний из двух async-колбэков увидит и членство, и выбор.
            applySpawn(*player);
        },
        [](const std::string &error)
        { LogManager::log(Error, "SpawnChoiceSystem: failed to load spawn choice: " + error); });
}

// ------------------------------------------------------------------ диалог /setspawn

void SpawnChoiceSystem::showDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Точка спавна");
    // Первая жёлтая строка — подсказка о смысле (настройка будущих спавнов, не
    // телепорт), далее три пункта. Индексы смещены на HINT_ROWS в колбэке.
    dialog.body = u("{FFB400}Где вы будете появляться при входе в игру и после смерти\n"
                    "Порт\n"
                    "Дом\n"
                    "Место работы");
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;

            // Выбор привязан к серверной сессии (accountId из неё, не от клиента).
            const PlayerSessionService::Session *session = m_sessionService.get(playerId);
            if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
                return; // без сессии выбору некуда сохраняться

            // Смещение на жёлтую подсказку: реальные пункты начинаются с HINT_ROWS.
            const int item = listItem - HINT_ROWS;
            switch (item)
            {
            case 0: // Порт (дефолт) — доступен всегда
                m_choiceService.setChoice(*player, session->accountId, SpawnChoiceService::Choice::Station);
                applySpawn(*player);
                player->sendClientMessage(INFO_COLOUR, u("Теперь вы появляетесь в порту"));
                break;
            case 1: // Дом — требует владения домом (серверная проверка)
            {
                if (!m_houseService.houseOf(std::to_string(session->accountId)))
                {
                    player->sendClientMessage(ERROR_COLOUR, u("У вас нет своего дома. Займите свободный дом на карте"));
                    return; // выбор НЕ меняем
                }
                m_choiceService.setChoice(*player, session->accountId, SpawnChoiceService::Choice::Home);
                applySpawn(*player);
                player->sendClientMessage(INFO_COLOUR, u("Теперь вы появляетесь у своего дома"));
                break;
            }
            case 2: // Место работы — база фракции (член орга с заданной точкой)
            {
                const int factionId = m_factionService.getMemberFaction(playerId);
                const FactionService::Faction *faction = m_factionService.getFaction(factionId);
                if (factionId == FactionService::NO_FACTION || !faction || !faction->spawn.defined)
                {
                    player->sendClientMessage(ERROR_COLOUR, u("Вы не состоите в организации"));
                    return; // выбор НЕ меняем
                }
                m_choiceService.setChoice(*player, session->accountId, SpawnChoiceService::Choice::Work);
                applySpawn(*player);
                player->sendClientMessage(INFO_COLOUR, u("Теперь вы появляетесь на месте работы"));
                break;
            }
            default:
                break; // подсказка или вне диапазона — игнор
            }
        });
}

// ------------------------------------------------------------------ применение спавна

void SpawnChoiceSystem::applySpawn(IPlayer &player)
{
    // Выбор главнее фракции: единственный писатель точки спавна — этот метод
    // (FactionSystem её не форсит). Единственный источник правды о спавне —
    // PlayerSpawnService. setSpawn задаёт точку для ВСЕХ последующих спавнов
    // (вход/смерть), НЕ телепортирует сейчас.
    m_spawnService.setSpawn(player, resolveSpawn(player));
}

SpawnPoint SpawnChoiceSystem::resolveSpawn(IPlayer &player) const
{
    const int playerId = player.getID();

    // Порт — дефолт и фолбэк для пропавших источников.
    SpawnPoint station;
    station.position = STATION_POSITION;
    station.angle = STATION_ANGLE;
    station.interior = 0;
    station.virtualWorld = 0;

    switch (m_choiceService.getChoice(playerId))
    {
    case SpawnChoiceService::Choice::Home:
    {
        // Дом резолвится по серверному владению (accountId из сессии). Резолв
        // выполняется на логине, /setspawn и смене членства, НЕ на каждой смерти:
        // точка замораживается в class-данных клиента до следующего applySpawn.
        // Если дом пропал в середине сессии (дев удалил дом), игрок с выбором Home
        // будет появляться у входа БЫВШЕГО дома (публичная точка, vw 0) до
        // следующего applySpawn — фолбэк на порт (дефолт) срабатывает на следующем
        // resolveSpawn. Нет сессии -> фолбэк на порт (дефолт).
        const PlayerSessionService::Session *session = m_sessionService.get(playerId);
        if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
            return station;
        const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
        if (!house)
            return station;
        SpawnPoint point;
        point.position = house->entrance; // вход дома (основной мир, интерьер 0)
        point.angle = 0.0f;
        point.interior = 0;
        point.virtualWorld = 0;
        return point;
    }
    case SpawnChoiceService::Choice::Work:
    {
        // База фракции игрока (серверные факты: членство + точка спавна орга).
        // Не член / у фракции нет точки -> фолбэк на порт (дефолт). Так экс-член,
        // вышедший из орга, перерезолвится на порт (дефолт) (см. подписку на member-change),
        // а не останется в приватном vw/интерьере базы.
        const FactionService::Faction *faction = m_factionService.getFaction(m_factionService.getMemberFaction(playerId));
        if (!faction || !faction->spawn.defined)
            return station;
        SpawnPoint point;
        point.position = faction->spawn.position;
        point.angle = faction->spawn.angle;
        point.interior = static_cast<unsigned>(faction->spawn.interior);
        point.virtualWorld = faction->spawn.virtualWorld;
        return point;
    }
    case SpawnChoiceService::Choice::Station:
    default:
        return station;
    }
}
