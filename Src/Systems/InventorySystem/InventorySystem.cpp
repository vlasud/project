#include "Systems/InventorySystem/InventorySystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <utility>
#include <vector>

namespace
{
// Подтверждение дев-выдачи — ADMIN_COLOUR (как весь дев/админ-тулинг).
const Colour ADMIN_COLOUR = Colour::FromRGBA(0xFFB400FF);
const Colour ERROR_COLOUR{255, 90, 90};

// Разумный предел разового ввода количества в дев-меню. add всё равно клампит к
// maxStack предмета — это лишь отсечка абсурдного клиентского ввода до него.
constexpr int MAX_DEV_GRANT = 10000;
} // namespace

InventorySystem::InventorySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_inventoryService(serviceRegister.getService<InventoryService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadItems(player, session);
        });
    // Персист в save-канал: идемпотентный REPLACE снимка. Зовётся и на конце
    // сессии (внутри end, до teardown), и периодически автосейвом.
    m_sessionService.subscribeSave(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            persistItems(player, session);
        });

    // Единое дев-меню выдачи вещей; доступ только разработчику (admin level 6) —
    // иначе любой игрок выдавал бы себе предметы. Скрыта из /help (для дева — /ahelp).
    serviceRegister.getService<PlayerCommandService>().add(
        "idev", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showDevMenu(player); },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "дев-меню вещей (меню)",
        PlayerCommandService::HelpCategory::Hidden);
}

void InventorySystem::onPlayerConnect(IPlayer &player)
{
    m_inventoryService.reset(player.getID());
    m_loaded[player.getID()] = false; // загрузка ещё не выполнялась
}

void InventorySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_inventoryService.reset(player.getID());
    m_loaded[player.getID()] = false;
}

void InventorySystem::loadItems(IPlayer &player, const PlayerSessionService::Session &session)
{
    // Запрос И вычитка строк — на воркере; на главный поток приходит владеющий
    // вектор пар (item_type, quantity). Отсутствие строк = пустой инвентарь
    // (reset на коннекте уже обнулил слот).
    DatabaseManager::selectQuery<std::vector<std::pair<int, int>>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("player_items")
                                         .select("item_type", "quantity")
                                         .where("account_id = :account")
                                         .bind("account", accountId)
                                         .execute();
            std::vector<std::pair<int, int>> result;
            // item_type/quantity читаем как int64 и сужаем: значение вне диапазона
            // int не должно ронять get<int>() (битый ряд иначе оборвал бы загрузку).
            // Незарегистрированный тип/мусор отбрасывает loadItems, лишнее клампит.
            while (mysqlx::Row row = rows.fetchOne())
                result.emplace_back(static_cast<int>(row.get(0).get<std::int64_t>()),
                                    static_cast<int>(row.get(1).get<std::int64_t>()));
            return result;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<std::pair<int, int>> rows)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия —
            // чужие вещи не должны прилететь.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            if (!m_core.getPlayers().get(playerId))
                return;

            // loadItems валидирует реестром (неизвестный тип — отбрасывает) и КЛАМПИТ
            // количество к maxStack: мусор/устаревший избыток из БД не осядет.
            m_inventoryService.loadItems(playerId, std::move(rows));

            // Загрузка дошла (даже если строк не было — это «пустой инвентарь»,
            // валидно). Теперь конец сессии вправе персистить, не рискуя затереть БД.
            m_loaded[playerId] = true;
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "InventorySystem: failed to load items: " + error);
        });
}

void InventorySystem::persistItems(IPlayer &player, const PlayerSessionService::Session &session)
{
    const int playerId = player.getID();
    // Не персистим, если загрузка не завершилась успешно: пустой слот затёр бы
    // реальные вещи в БД (сбой загрузки/дисконнект до её колбэка).
    if (!m_loaded[playerId])
        return;

    // Снимок ненулевых количеств на главном потоке (источник правды — сервис),
    // затем одна задача-воркер переписывает строки аккаунта. Сессия ещё активна,
    // accountId валиден.
    std::vector<std::pair<int, int>> snapshot = m_inventoryService.snapshot(playerId);

    // Упорядочено по аккаунту: снимок абсолютный, а save-канал может отработать
    // дважды по одному игроку в одном стеке (см. PlayerMoneyPersistSystem::
    // persistMoney) и на автосейве рядом с концом сессии. Сегодня оба снимка
    // совпадают — ключ держит это верным и когда между ними что-то изменится.
    DatabaseManager::throwQueryOrdered(
        fmt::format("player_items:{}", session.accountId),
        [accountId = session.accountId, snapshot = std::move(snapshot)](mysqlx::Schema schema)
        {
            // REPLACE снимка В ОДНОЙ ТРАНЗАКЦИИ: удаляем все строки аккаунта и
            // вставляем текущие ненулевые атомарно. Так пропавшие предметы (потрачены
            // за сессию) исчезают из БД, а нулевые количества не хранятся (quantity > 0
            // гарантирован snapshot'ом). Транзакция убирает окна отдельных авто-коммитов:
            //  - видимое «ноль строк» между DELETE и INSERT (параллельная загрузка при
            //    релоге прочла бы пустой инвентарь — потеря);
            //  - частичную запись, если воркер упадёт между удалением и частью вставок.
            // Либо всё, либо ничего; при сбое — rollback, БД остаётся в прежнем виде.
            mysqlx::Table table = schema.getTable("player_items");
            mysqlx::Session &dbSession = schema.getSession();
            dbSession.startTransaction();
            try
            {
                table.remove().where("account_id = :account").bind("account", accountId).execute();
                for (const auto &[itemType, qty] : snapshot)
                    table.insert("account_id", "item_type", "quantity").values(accountId, itemType, qty).execute();
                dbSession.commit();
            }
            catch (...)
            {
                dbSession.rollback(); // не оставляем аккаунт с частичным инвентарём
                throw;                // errorCallback залогирует; сессия вернётся в пул чистой
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "InventorySystem: failed to persist items: " + error);
        });
}

// ------------------------------------------------------------------ дев-меню /idev

void InventorySystem::showDevMenu(IPlayer &player)
{
    const std::vector<InventoryService::ItemDef> &items = m_inventoryService.registeredItems();

    // Один предмет на строку body (в порядке регистрации); listItem диалога = индекс
    // в этом же векторе. Новые предметы появятся новыми строками автоматически.
    std::string body;
    for (const InventoryService::ItemDef &def : items)
        body += def.name + "\n";

    Dialog dialog;
    dialog.style = DialogStyle_LIST;
    dialog.title = u("Вещи — дев-меню");
    dialog.body = u(body);
    dialog.leftButton = u("Выбрать");
    dialog.rightButton = u("Закрыть");

    m_dialogService.show(
        player, dialog,
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
                return;
            const std::vector<InventoryService::ItemDef> &items = m_inventoryService.registeredItems();
            if (listItem < 0 || listItem >= static_cast<int>(items.size()))
                return; // клиент мог прислать индекс вне списка
            showDevAmountInput(*player, items[static_cast<std::size_t>(listItem)].itemType);
        });
}

void InventorySystem::showDevAmountInput(IPlayer &player, int itemType)
{
    // Имя предмета в заголовок — дев видит, ЧТО выдаёт. Если тип вдруг не
    // зарегистрирован (гонка перерегистрации/мусор) — назад в меню.
    const InventoryService::ItemDef *def = nullptr;
    for (const InventoryService::ItemDef &d : m_inventoryService.registeredItems())
        if (d.itemType == itemType)
        {
            def = &d;
            break;
        }
    if (!def)
    {
        showDevMenu(player);
        return;
    }
    const std::string itemName = def->name;

    Dialog dialog;
    dialog.style = DialogStyle_INPUT;
    dialog.title = u(fmt::format("Выдача: {}", itemName));
    dialog.body = u("Введите количество");
    dialog.leftButton = u("Выдать");
    dialog.rightButton = u("Назад");

    m_dialogService.showNumberInput(
        player, dialog,
        [this, playerId = player.getID(), itemType, itemName](DialogResponse response, std::int64_t amount)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
                return;
            if (response != DialogResponse_Left)
            {
                // «Назад» — возврат в меню выбора, не закрытие.
                showDevMenu(*player);
                return;
            }
            // Клиентский ввод: целое > 0 и в разумном пределе. Мусор/пусто/overflow
            // сервис уже отсёк повторным показом; здесь проверяем диапазон значения.
            if (amount < 1 || amount > MAX_DEV_GRANT)
            {
                player->sendClientMessage(ERROR_COLOUR, u("Некорректное количество"));
                showDevAmountInput(*player, itemType);
                return;
            }
            const int added = m_inventoryService.add(playerId, itemType, static_cast<int>(amount));
            const int total = m_inventoryService.count(playerId, itemType);
            player->sendClientMessage(
                ADMIN_COLOUR, u(fmt::format("Выдано: {} × {}. Всего: {}", added, itemName, total)));
        });
}
