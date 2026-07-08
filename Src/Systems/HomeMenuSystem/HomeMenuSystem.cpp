#include "Systems/HomeMenuSystem/HomeMenuSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <string>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

Dialog makeDialog(DialogStyle style, const std::string &title, const std::string &body, const std::string &leftButton,
                  const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = u(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}
} // namespace

HomeMenuSystem::HomeMenuSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_houseService(serviceRegister.getService<HouseService>()),
      m_parkedService(serviceRegister.getService<ParkedVehicleService>()),
      m_personalService(serviceRegister.getService<PersonalVehicleService>()),
      m_vehicleService(serviceRegister.getService<VehicleService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_waypointService(serviceRegister.getService<VehicleWaypointService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("home", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showRoot(player); },
                 {}, "меню вашего дома: информация, карта, передача, выселение",
                 PlayerCommandService::HelpCategory::Misc);
}

// ---------------------------------------------------------------- корень /home

void HomeMenuSystem::showRoot(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет своего дома. Займите свободный дом на карте"));
        return;
    }
    const char *name =
        HouseService::catalogValid(house->interiorIndex) ? m_houseService.catalog()[house->interiorIndex].name : "?";

    // Все 6 пунктов видны ВСЕГДА (правило видимости) — недоступность объясняет
    // сам обработчик сообщением при клике.
    enum RootItem
    {
        ItemInfo = 0,
        ItemMap,
        ItemUpgrades,
        ItemTransfer,
        ItemSell,
        ItemEvict,
    };
    std::string body;
    body += "Информация\n";
    body += "Отметить на карте\n";
    body += "Улучшения\n";
    body += "Передать владение\n";
    body += "Продать\n";
    body += "Выселиться";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Мой дом — {}", name), body, "Выбрать", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            switch (listItem)
            {
            case ItemInfo:
                showInfo(*player);
                break;
            case ItemMap:
                showOnMap(*player);
                break;
            case ItemUpgrades:
                showUpgrades(*player);
                break;
            case ItemTransfer:
                showTransferInput(*player);
                break;
            case ItemSell:
                showSellStub(*player);
                break;
            case ItemEvict:
                showEvictConfirm(*player);
                break;
            default:
                break;
            }
        });
}

// ---------------------------------------------------------------- 1: информация

void HomeMenuSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }
    const char *name =
        HouseService::catalogValid(house->interiorIndex) ? m_houseService.catalog()[house->interiorIndex].name : "?";
    const std::size_t parkedNow = m_parkedService.countParkedByAccount(session->accountId);

    // TABLIST_HEADERS «Поле | Значение» (образец — MenuSystem::showInfo). Раздел
    // расширяемый — новые факты о доме добавляются новыми строками.
    std::string body = "Поле\tЗначение\n";
    body += fmt::format("Тип дома\t{}\n", name);
    body += fmt::format("Лимит машин у дома\t{}\n", house->parkingCap);
    body += fmt::format("Припарковано сейчас\t{}", parkedNow);

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Информация о доме", body, "Назад", ""),
        [this, playerId](DialogResponse, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (player)
                showRoot(*player);
        });
}

// ---------------------------------------------------------------- 2: отметить на карте

void HomeMenuSystem::showOnMap(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    const HouseService::House *house = m_houseService.houseOf(std::to_string(session->accountId));
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }

    // Статическая точка (не машина) — VehicleWaypointService::showFor(Vector3).
    // Единый чекпоинт-слот игрока; НЕ трогаем CheckpointService напрямую (два
    // хозяина слота гасили бы чекпоинты друг друга).
    m_waypointService.showFor(player, house->entrance);
    player.sendClientMessage(INFO_COLOUR, u("Ваш дом отмечен на карте красным чекпоинтом"));
}

// ---------------------------------------------------------------- 3: улучшения (заглушка)

void HomeMenuSystem::showUpgrades(IPlayer &player)
{
    player.sendClientMessage(INFO_COLOUR, u("Улучшения пока в разработке"));
    showRoot(player);
}

// ---------------------------------------------------------------- 5: продать (заглушка)

void HomeMenuSystem::showSellStub(IPlayer &player)
{
    player.sendClientMessage(INFO_COLOUR, u("Продажа домов пока в разработке"));
    showRoot(player);
}

// ---------------------------------------------------------------- общие хелперы передачи/выселения

bool HomeMenuSystem::anyParkedOccupied(PlayerSessionService::AccountId accountId) const
{
    for (const long long dbId : m_parkedService.parkedByAccount(accountId))
    {
        const ParkedVehicleService::Parked *parked = m_parkedService.byDbId(dbId);
        if (parked && parked->vehicleId != -1 && m_vehicleService.getDriver(parked->vehicleId) != -1)
        {
            return true;
        }
    }
    return false;
}

void HomeMenuSystem::unparkAllOf(int playerId, PlayerSessionService::AccountId accountId)
{
    // Снимок списка ДО цикла: unpark правит m_byAccount изнутри, перебор живого
    // индекса инвалидировался бы (как в ParkedVehicleService::onOwnerLeftFamily).
    const std::vector<long long> dbIds = m_parkedService.parkedByAccount(accountId);
    const std::vector<PersonalVehicleService::OwnedVehicle> &owned = m_personalService.owned(playerId);
    for (const long long dbId : dbIds)
    {
        // Снять актуальный остаток бака В ВЛАДЕНИЕ ДО destroy (unpark уничтожает
        // экземпляр — parked_vehicle.fuel улетает вместе со строкой). Живой экземпляр
        // ещё существует -> берём фактический getFuel; иначе (машина сейчас деспавнена,
        // владелец оффлайн) — последний снимок parked->fuel. Без этого следующий спавн
        // через центральную парковку взял бы устаревший (до-парковочный) entry.fuel —
        // как штатный CarMenuSystem::unpark, только по каждой записи владельца.
        const ParkedVehicleService::Parked *parked = m_parkedService.byDbId(dbId);
        if (parked)
        {
            const float fuel =
                parked->vehicleId != -1 ? m_vehicleService.getFuel(parked->vehicleId) : parked->fuel;
            for (std::size_t i = 0; i < owned.size(); ++i)
            {
                if (owned[i].dbId == dbId)
                {
                    m_personalService.setFuel(playerId, static_cast<int>(i), fuel);
                    break;
                }
            }
        }
        m_parkedService.unpark(dbId); // уничтожает экземпляр + DELETE строки (шеринг уходит вместе с записью)
    }
}

// ---------------------------------------------------------------- 4: передать владение

void HomeMenuSystem::showTransferInput(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    if (!m_houseService.houseOf(std::to_string(session->accountId)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Передать владение",
                   "Введите id игрока (посмотреть можно по TAB), которому хотите передать дом.\n"
                   "Он должен быть в сети, авторизован и не иметь своего дома.",
                   "Далее", "Назад"),
        [this, playerId](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showRoot(*player);
                return;
            }
            // Парс уже дал целое; bounds к диапазону id игроков — иначе getPlayers().get
            // получит мусорный id (bounds-safe само API, но лучше явный отказ раньше).
            if (value < 0 || value >= MAX_PLAYERS)
            {
                // Мусор/вне диапазона — это ошибка ВВОДА, не «нет такого игрока»:
                // разные причины — разные тексты.
                player->sendClientMessage(ERROR_COLOUR, u("Введите корректный id игрока"));
                showTransferInput(*player);
                return;
            }
            showTransferConfirm(*player, static_cast<int>(value));
        });
}

void HomeMenuSystem::showTransferConfirm(IPlayer &player, int recipientId)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    if (!m_houseService.houseOf(std::to_string(session->accountId)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }
    if (recipientId == playerId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Нельзя передать дом самому себе"));
        showRoot(player);
        return;
    }
    IPlayer *recipient = m_core.getPlayers().get(recipientId);
    if (!recipient)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Такого игрока нет в сети"));
        showRoot(player);
        return;
    }
    const PlayerSessionService::AccountId recipientAccount = m_sessionService.getAccountId(recipientId);
    if (recipientAccount == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок ещё не авторизован"));
        showRoot(player);
        return;
    }
    if (m_houseService.ownsHouse(std::to_string(recipientAccount)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У этого игрока уже есть свой дом"));
        showRoot(player);
        return;
    }

    const std::string recipientName = recipient->getName().to_string();
    const std::string body =
        fmt::format("Передать дом {}[{}]?\nВсе припаркованные машины отправятся на парковку. Это действие "
                    "нельзя отменить.",
                    recipientName, recipientId);

    m_dialogService.show(
        player, makeDialog(DialogStyle_MSGBOX, "Передача дома", body, "Передать", "Назад"),
        [this, playerId, recipientId, recipientAccount](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showRoot(*player);
                return;
            }
            executeTransfer(*player, recipientId, recipientAccount);
        });
}

void HomeMenuSystem::executeTransfer(IPlayer &player, int recipientId,
                                     PlayerSessionService::AccountId expectedAccount)
{
    const int playerId = player.getID();

    // Полная ре-валидация на момент согласия (диалог мог висеть, пока всё менялось).
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    const PlayerSessionService::AccountId ownerAccount = session->accountId;
    const std::string ownerKey = std::to_string(ownerAccount);
    const HouseService::House *house = m_houseService.houseOf(ownerKey);
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }
    if (recipientId == playerId)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Нельзя передать дом самому себе"));
        showRoot(player);
        return;
    }
    IPlayer *recipient = m_core.getPlayers().get(recipientId);
    if (!recipient)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Такого игрока нет в сети"));
        showRoot(player);
        return;
    }
    const PlayerSessionService::AccountId recipientAccount = m_sessionService.getAccountId(recipientId);
    if (recipientAccount == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок ещё не авторизован"));
        showRoot(player);
        return;
    }
    // Слот id мог занять ДРУГОЙ игрок, пока подтверждение висело — дом уходит
    // только тому аккаунту, чьё имя показывали в окне.
    if (recipientAccount != expectedAccount)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот игрок сменился — начните передачу заново"));
        showRoot(player);
        return;
    }
    const std::string recipientKey = std::to_string(recipientAccount);
    if (m_houseService.ownsHouse(recipientKey))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У этого игрока уже есть свой дом"));
        showRoot(player);
        return;
    }

    // Гейт занятости: под сидящим водителем destroy не делаем.
    if (anyParkedOccupied(ownerAccount))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Пока в припаркованных машинах есть водители, передача недоступна"));
        showRoot(player);
        return;
    }

    const int houseId = house->id;
    unparkAllOf(playerId, ownerAccount); // все машины владельца -> на парковку (со снимком fuel)

    // Смена владельца — источник правды (память), персист/иконку доводит
    // HouseSystem по подписке subscribeOwnerChanged (единая точка).
    m_houseService.setOwner(houseId, recipientKey);

    const std::string recipientName = recipient->getName().to_string();
    const std::string ownerName = player.getName().to_string();
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Дом передан {}[{}]", recipientName, recipientId)));
    recipient->sendClientMessage(
        INFO_COLOUR, u(fmt::format("{}[{}] передал вам дом — теперь это ваш дом", ownerName, playerId)));
}

// ---------------------------------------------------------------- 6: выселиться

void HomeMenuSystem::showEvictConfirm(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    if (!m_houseService.houseOf(std::to_string(session->accountId)))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_MSGBOX, "Выселение",
                   "Выселиться из дома? Дом станет ничейным, припаркованные машины отправятся на парковку. "
                   "Это действие нельзя отменить.",
                   "Выселиться", "Назад"),
        [this, playerId](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showRoot(*player);
                return;
            }
            executeEvict(*player);
        });
}

void HomeMenuSystem::executeEvict(IPlayer &player)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы ещё не авторизованы"));
        return;
    }
    const PlayerSessionService::AccountId ownerAccount = session->accountId;
    const HouseService::House *house = m_houseService.houseOf(std::to_string(ownerAccount));
    if (!house)
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас больше нет дома"));
        return;
    }

    if (anyParkedOccupied(ownerAccount))
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u("Пока в припаркованных машинах есть водители, выселение недоступно"));
        showRoot(player);
        return;
    }

    const int houseId = house->id;
    unparkAllOf(playerId, ownerAccount); // все машины владельца -> на парковку (со снимком fuel)
    m_houseService.setOwner(houseId, ""); // персист/иконку доводит HouseSystem (subscribeOwnerChanged)

    player.sendClientMessage(INFO_COLOUR, u("Вы выселились. Дом снова свободен"));
}
