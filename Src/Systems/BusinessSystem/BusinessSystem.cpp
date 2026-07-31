#include "Systems/BusinessSystem/BusinessSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/Geometry/Geometry.h"
#include "Utils/MoneyFormat/MoneyFormat.h"
#include "Utils/TimeFormat/TimeFormat.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
const Colour DEV_COLOUR{200, 200, 200};

const std::string BUSINESSES_FILE = "businesses.json"; // рабочая директория сервера
const std::string BUSINESSES_BACKUP = "businesses.json.bak";

// Ключ упорядочивания записей business_owner по бизнесу (DatabaseManager::
// throwQueryOrdered). Занятие/передача (DELETE+INSERT транзакцией) и снятие владения
// спорят за ОДНУ строку business_id: без ключа их коммиты могут лечь в обратном
// порядке, и владение в БД разойдётся с памятью. Бизнес, а не аккаунт: business_id —
// первичный ключ строки, «один бизнес на аккаунт» держит UNIQUE(account_id) схемы.
std::string ownershipKey(int businessId)
{
    return fmt::format("business_owner:{}", businessId);
}

// Закупочная цена товара — доля от его цены продажи. Разница между 100% и этим
// числом и есть маржа точки: продал за 250, закупил за 50, заработал 200.
constexpr std::int64_t ORDER_PRICE_PERCENT = 20;

// Насколько близко должен стоять покупатель/получатель при продаже и передаче.
// Сделка «через полкарты» — типичный способ развода, поэтому обе стороны обязаны
// видеть друг друга. Дистанция считается по ПРИНЯТЫМ сервером позициям.
constexpr float HANDOVER_RADIUS = 5.0f;

// Попап при входе в точку: короткая метка типа («24/7»), без подробностей.
constexpr Milliseconds ENTER_POPUP_TIME{2500};
// Звук входа в магазин SA.
constexpr std::uint32_t ENTER_SOUND = 6401;

// Радиус чекпоинта-прилавка внутри интерьера. Маленький: это касса, а не зона —
// витрина должна открываться, когда игрок реально подошёл к ней, а не проходя мимо.
constexpr float COUNTER_CHECKPOINT_RADIUS = 1.0f;

// Пикапы: белая стрелка снаружи (вход) и она же внутри (выход).
constexpr int ENTRANCE_PICKUP_MODEL = 1318;
constexpr int EXIT_PICKUP_MODEL = 1318;
constexpr PickupType PICKUP_TYPE = 1;

// Стабильный ключ категории в файле аукционов. Строка, а не индекс регистрации:
// порядок систем правится, а ставки обязаны оставаться на своих лотах.
const std::string AUCTION_KEY = "business";

// Иконка бизнеса на карте: 52 — «магазин». Стрим — по радару (общая константа).
constexpr int BUSINESS_MAP_ICON = 52;

// 3D-текст у входа — игроцкий ориентир точки (виден всем у двери): тип бизнеса и
// его номер. Тёплый золотой в пару к торговой иконке; НЕ белый (выгорает на песке).
// 10 м + testLOS=true: текст читается только вблизи и НЕ проступает сквозь стены.
const Colour BUSINESS_LABEL_COLOUR{255, 205, 100};
constexpr float BUSINESS_LABEL_DRAW_DISTANCE = 10.0f;
constexpr bool BUSINESS_LABEL_TEST_LOS = true;

// Пикап выхода смещён от точки спавна внутри: иначе игрок появлялся бы прямо в нём
// и мгновенно выходил обратно.
// Сколько метров ЗА СПИНОЙ вошедшего стоит пикап выхода из интерьера. Выходы
// СЧИТАЮТСЯ, а не замеряются (политика проекта); обоснование дистанции и правила —
// в HouseSystem, константа держится одинаковой у домов и бизнесов.
constexpr float INSIDE_EXIT_DISTANCE = 2.0f;

// Потолок цены при создании: защита от опечатки дева. Он же потолок ставки —
// выше денег в игре всё равно не бывает, а int64 гарантированно не переполнится.
constexpr std::int64_t MAX_PRICE = 100000000;

// Разобрать businesses.json — ОПИСАНИЕ точек (дев-контент). Ни владение, ни ставки
// здесь не читаются: и то и другое — динамика, её место в БД.
// ok=false — файл битый (вызывающий уводит его в .bak).
std::vector<BusinessService::Business> parse(const std::string &content, bool &ok)
{
    std::vector<BusinessService::Business> parsed;
    ok = false;
    try
    {
        const nlohmann::json root = nlohmann::json::parse(content);
        // Формат файла — объект с секциями (бизнесы + возвраты). Голый массив —
        // файл от сборки до аукциона, читаем как список бизнесов.
        const nlohmann::json *list = nullptr;
        if (root.is_array())
        {
            list = &root;
        }
        else if (root.is_object() && root.contains("businesses") && root["businesses"].is_array())
        {
            list = &root["businesses"];
        }
        else
        {
            return parsed; // ни массив, ни ожидаемый объект — считаем файл битым
        }

        for (const nlohmann::json &item : *list)
        {
            if (!item.is_object() || !item.contains("id") || !item.contains("entrance"))
            {
                continue; // мусорная запись — пропускаем, файл в целом валиден
            }
            BusinessService::Business business;
            business.id = item.value("id", 0);
            const int typeValue = item.value("type", 0);
            if (typeValue < 0 || typeValue >= static_cast<int>(BusinessService::Type::Count))
            {
                continue; // неизвестный тип (файл от новой сборки) — запись пропускаем
            }
            business.type = static_cast<BusinessService::Type>(typeValue);
            business.interiorIndex = item.value("interior", 0);
            const auto entrance = item["entrance"];
            const auto exit = item.value("exit", nlohmann::json::array({0.0f, 0.0f, 0.0f}));
            if (!entrance.is_array() || entrance.size() != 3 || !exit.is_array() || exit.size() != 3)
            {
                continue;
            }
            business.entrance = Vector3(entrance[0].get<float>(), entrance[1].get<float>(), entrance[2].get<float>());
            business.exit = Vector3(exit[0].get<float>(), exit[1].get<float>(), exit[2].get<float>());
            business.exitAngle = item.value("exitAngle", 0.0f);
            business.virtualWorld = item.value("vw", BusinessService::VW_BASE + business.id);
            business.price = item.value("price", static_cast<std::int64_t>(0));
            business.balance = item.value("balance", static_cast<std::int64_t>(0));
            // owner/bids из файла НЕ читаем даже если они там есть (наследие сборки
            // до переезда динамики в БД): их источник — business_owner и auction_*.
            parsed.push_back(std::move(business));
        }
        ok = true;
    }
    catch (const std::exception &)
    {
        ok = false; // битый json — вызывающий уводит файл в .bak
    }
    return parsed;
}
} // namespace

BusinessSystem::BusinessSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_businessService(serviceRegister.getService<BusinessService>()),
      m_auctionService(serviceRegister.getService<AuctionService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_mapIconService(serviceRegister.getService<MapIconService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_cameraService(serviceRegister.getService<CameraService>()),
      m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_noticeService(serviceRegister.getService<ScreenNoticeService>()),
      m_audioService(serviceRegister.getService<AudioService>()),
      m_inventoryService(serviceRegister.getService<InventoryService>()),
      m_orderService(serviceRegister.getService<BusinessOrderService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_labelService(serviceRegister.getService<TextLabelService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    // Дев-инструмент переехал на /dbusiness: /business занял ВЛАДЕЛЕЦ — команда
    // игрока встречается несравнимо чаще, и короткое имя должно быть у неё.
    commands.add(
        "dbusiness", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showDevMenu(player);
        },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "бизнесы — дев-меню (создание/список/снос)",
        PlayerCommandService::HelpCategory::Hidden);

    commands.add(
        "business", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showManageMenu(player);
        },
        {}, "управление своим бизнесом", PlayerCommandService::HelpCategory::Economy);

    // Любое изменение описания (создание/снос/копилка) пишет файл — единая точка
    // персиста, чтобы ни один путь изменения не забыл сохранить.
    m_businessService.subscribeChanged(
        [this]()
        {
            saveToFileAsync();
        });

    // История выручки по дням — в БД: копилка показывает «сколько лежит сейчас», а
    // владельцу нужно «сколько принесла точка за неделю».
    m_businessService.subscribeIncome(
        [this](int businessId, std::int64_t income)
        {
            persistIncomeDay(businessId, income);
        });

    // Склад — динамика, значит БД (json дев-контент и в проде read-only).
    m_businessService.subscribeStockChanged(
        [this](int businessId, int itemType, int quantity)
        {
            persistStock(businessId, itemType, quantity);
        });

    // Заказ держит УЖЕ СПИСАННЫЕ у владельца деньги — он обязан пережить рестарт.
    m_orderService.subscribeChanged([this](int orderId) { persistOrder(orderId); });

    // Черновик заказа привязан к слоту игрока — на конце сессии он чужой.
    m_sessionService.subscribeEnd([this](IPlayer &player, const PlayerSessionService::Session &)
                                  { clearOrderDraft(player.getID()); });

    // Владение — в БД, не в файле: ЕДИНАЯ точка write-through для всех путей
    // смены владельца (итог аукциона сейчас, передача/выселение потом).
    m_businessService.subscribeOwnerChanged(
        [this](int businessId, const std::string &oldKey, const std::string &newKey)
        {
            onOwnerChanged(businessId, oldKey, newKey);
        });

    // Категория «Бизнесы» в общих торгах. Госимущество через неё БОЛЬШЕ НЕ ПРОДАЁТСЯ
    // (бизнес покупается по госцене на пикапе), но регистрация остаётся: уцелевшие с
    // прежней схемы лоты должны на своём сроке вернуть ставки, а не выдать бизнес, и
    // будущая продажа имущества игроками переиспользует этот же ключ. Торги ведёт
    // отсюда только «как выглядит лот», «кому можно отдать» и «как передать».
    AuctionService::CategoryDef category;
    category.key = AUCTION_KEY;
    category.name = "Бизнесы";
    category.info = [this](int businessId, AuctionService::Lot &out)
    {
        return describeLot(businessId, out);
    };
    category.eligible = [this](const std::string &ownerKey)
    {
        return !m_businessService.ownsBusiness(ownerKey); // один бизнес в одни руки
    };
    category.award = [this](int businessId, const std::string &ownerKey)
    {
        // setOwner дёрнет onOwnerChanged — тот и запишет владение в БД.
        m_businessService.setOwner(businessId, ownerKey);
    };
    category.ready = [this]()
    {
        // Пока зеркало владения не пришло из БД, «один бизнес на аккаунт» проверять
        // нечем, а раздача бизнеса необратима.
        return m_businessService.isOwnershipLoaded();
    };
    m_auctionService.registerCategory(std::move(category));
}

void BusinessSystem::initialize(IComponentList * /*components*/)
{
    registerCounterCheckpoints();
    loadFromFileAsync();
    loadStockAsync();
    loadOrdersAsync();
}

// Чекпоинт-ПРИЛАВОК: встал на него внутри точки — открылась витрина типа.
//
// ОДИН чекпоинт на ИНТЕРЬЕР, а не на бизнес. Глобальные чекпоинты не фильтруются
// по виртуальному миру (CheckpointService::streamPlayer выбирает ближайший по
// расстоянию), а все точки одного интерьера стоят в ОДНИХ координатах и различаются
// только vw. Чекпоинт на каждый бизнес дал бы стопку Def'ов в одной точке, и стрим
// выбирал бы из них произвольный — вплоть до чужого бизнеса. Поэтому регистрируем по
// одному на интерьер, а КОНКРЕТНУЮ точку определяем по vw вошедшего.
//
// В initialize, а не в конструкторе: типы бизнеса регистрируют себя в своих
// конструкторах, и полный список каталогов известен только после их прогона.
void BusinessSystem::registerCounterCheckpoints()
{
    for (const BusinessService::Type type : m_businessService.registeredTypes())
    {
        const std::vector<BusinessService::CatalogEntry> &catalog = m_businessService.catalog(type);
        for (std::size_t index = 0; index < catalog.size(); ++index)
        {
            const Vector3 &counter = catalog[index].counter;
            if (counter.x == 0.0f && counter.y == 0.0f && counter.z == 0.0f)
            {
                continue; // прилавок у интерьера не замерен — чекпоинта нет
            }
            m_checkpointService.add(counter, COUNTER_CHECKPOINT_RADIUS,
                                    [this, type, index](IPlayer &player)
                                    {
                                        onCounterEnter(player, type, index);
                                    });
        }
    }
}

void BusinessSystem::onCounterEnter(IPlayer &player, BusinessService::Type type, std::size_t interiorIndex)
{
    // Какой именно бизнес — по виртуальному миру игрока: у каждого он свой
    // (VW_BASE + id), и это серверный факт, а не клиентское заявление.
    const int virtualWorld = m_locationService.getVirtualWorld(player.getID());
    const int businessId = virtualWorld - BusinessService::VW_BASE;
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return; // игрок не внутри точки (совпадение координат в другом мире)
    }
    // Чекпоинт принадлежит конкретному интерьеру конкретного типа: в чужом бизнесе,
    // случайно совпавшем по координатам, витрину не открываем.
    if (business->type != type || static_cast<std::size_t>(business->interiorIndex) != interiorIndex)
    {
        return;
    }
    if (business->owner.empty())
    {
        return; // ничейная точка не работает (внутрь и не пускают)
    }
    m_businessService.openVisitorMenu(business->type, player, businessId);
}

// ------------------------------------------------------------------ дев-меню

void BusinessSystem::showDevMenu(IPlayer &player)
{
    const int playerId = player.getID();
    // ПОРЯДОК СТРОК = порядок case-веток обработчика.
    const std::string body =
        fmt::format("Создать бизнес\nСписок ({})\nСнести бизнес\nСклад точки", m_businessService.count());

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Бизнесы — дев-меню", body, "Выбрать", "Закрыть"),
                         [this, playerId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *dev = m_core.getPlayers().get(playerId);
                             if (!dev || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 showTypeChoice(*dev);
                                 break;
                             case 1:
                                 showDevList(*dev);
                                 break;
                             case 2:
                                 showDevRemove(*dev);
                                 break;
                             case 3:
                                 showDevStockPick(*dev);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void BusinessSystem::showTypeChoice(IPlayer &player)
{
    const int playerId = player.getID();
    const std::vector<BusinessService::Type> types = m_businessService.registeredTypes();
    if (types.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Ни один тип бизнеса не зарегистрирован"));
        return;
    }

    std::string body;
    for (const BusinessService::Type type : types)
    {
        body += m_businessService.typeName(type) + "\n";
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Тип бизнеса", body, "Далее", "Назад"),
        [this, playerId, types](DialogResponse response, int listItem, StringView)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showDevMenu(*dev);
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(types.size()))
            {
                return; // индекс от клиента — проверяем по снимку списка
            }
            showInteriorChoice(*dev, types[static_cast<std::size_t>(listItem)]);
        });
}

void BusinessSystem::showInteriorChoice(IPlayer &player, BusinessService::Type type)
{
    const int playerId = player.getID();
    const std::vector<BusinessService::CatalogEntry> &catalog = m_businessService.catalog(type);
    if (catalog.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("У этого типа нет интерьеров"));
        return;
    }

    std::string body = "Интерьер\tID\tX,Y,Z\n";
    for (const BusinessService::CatalogEntry &entry : catalog)
    {
        body += fmt::format("{}\t{}\t{:.2f},{:.2f},{:.2f}\n", entry.name, entry.interiorId, entry.insideSpawn.x,
                            entry.insideSpawn.y, entry.insideSpawn.z);
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Интерьер бизнеса", body, "Далее", "Назад"),
        [this, playerId, type](DialogResponse response, int listItem, StringView)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showTypeChoice(*dev);
                return;
            }
            if (!m_businessService.catalogValid(type, listItem))
            {
                return;
            }
            showPriceInput(*dev, type, listItem);
        });
}

void BusinessSystem::showPriceInput(IPlayer &player, BusinessService::Type type, int interiorIndex)
{
    const int playerId = player.getID();
    m_dialogService.show(
        player,
        // ГОСЦЕНА бизнеса: за неё игрок покупает его на месте, у пикапа входа
        // (см. Docs/Business.md). 0 — бизнес государством не выставлен.
        makeDialog(DialogStyle_INPUT, "Государственная цена",
                   "Сколько стоит этот бизнес у государства?\nИгрок платит эту сумму и сразу получает бизнес.\n"
                   "0 — бизнес не продаётся."
                   "\n\nБизнес создастся в ВАШЕЙ текущей точке.",
                   "Создать", "Назад"),
        [this, playerId, type, interiorIndex](DialogResponse response, int, StringView text)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showInteriorChoice(*dev, type);
                return;
            }
            const std::string entered(text.data(), text.size());
            char *end = nullptr;
            const long long price = std::strtoll(entered.c_str(), &end, 10);
            if (entered.empty() || end == entered.c_str() || *end != '\0' || price < 0 || price > MAX_PRICE)
            {
                dev->sendClientMessage(ERROR_COLOUR,
                                       u(fmt::format("Гос. цена — целое число от 0 до {}", MAX_PRICE)));
                return;
            }
            createBusiness(*dev, type, interiorIndex, static_cast<std::int64_t>(price));
        });
}

void BusinessSystem::createBusiness(IPlayer &player, BusinessService::Type type, int interiorIndex,
                                    std::int64_t price)
{
    const int playerId = player.getID();
    // Позиция создателя — ПРИНЯТАЯ сервером, угол берём с клиента (только ориентация
    // точки выхода, авторитетность тут не нужна).
    const Vector3 position = m_locationService.getPosition(playerId);
    const float angle = player.getRotation().ToEuler().z;

    const BusinessService::Business *business =
        m_businessService.createBusiness(type, position, angle, interiorIndex, price);
    if (!business)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Не удалось создать бизнес"));
        return;
    }
    spawnBusiness(*business);
    player.sendClientMessage(DEV_COLOUR,
                             u(fmt::format("Бизнес #{} создан: {} (гос. цена ${})", business->id,
                                           m_businessService.typeName(type), business->price)));
}

void BusinessSystem::showDevList(IPlayer &player)
{
    const int playerId = player.getID();
    if (m_businessService.count() == 0)
    {
        player.sendClientMessage(DEV_COLOUR, u("Бизнесов нет"));
        return;
    }

    // Снимок id в порядке показа: listItem клиента адресует именно его.
    // Колонки ставок больше нет: госимущество не разыгрывается, она всегда была бы
    // пустой и вводила бы дева в заблуждение.
    std::vector<int> ids;
    std::string body = "ID\tТип\tВладелец\tГос. цена\tКопилка\n";
    for (const auto &[id, business] : m_businessService.businesses())
    {
        ids.push_back(id);
        body += fmt::format("{}\t{}\t{}\t${}\t${}\n", id, m_businessService.typeName(business.type),
                            business.owner.empty() ? "—" : business.owner, business.price, business.balance);
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Бизнесы", body, "Гос. цена", "Закрыть"),
        [this, playerId, ids](DialogResponse response, int listItem, StringView)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev || response != DialogResponse_Left)
            {
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(ids.size()))
            {
                return; // индекс от клиента — по снимку, с которым строился список
            }
            showPriceEdit(*dev, ids[static_cast<std::size_t>(listItem)]);
        });
}

void BusinessSystem::showPriceEdit(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        player.sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} не найден", businessId)));
        return;
    }
    // Новая цена действует со следующей покупки; уже проданный бизнес она не трогает.
    const std::string title = fmt::format("Бизнес #{} — гос. цена", businessId);
    const std::string body =
        fmt::format("Сейчас: ${}\n\nСколько стоит этот бизнес у государства?\n"
                    "Игрок платит эту сумму и сразу получает бизнес.\n0 — бизнес не продаётся.",
                    business->price);

    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, title, body, "Сохранить", "Назад"),
        [this, playerId, businessId](DialogResponse response, int, StringView text)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showDevList(*dev);
                return;
            }
            const std::string entered(text.data(), text.size());
            char *end = nullptr;
            const long long price = std::strtoll(entered.c_str(), &end, 10);
            if (entered.empty() || end == entered.c_str() || *end != '\0' || price < 0 || price > MAX_PRICE)
            {
                dev->sendClientMessage(ERROR_COLOUR,
                                       u(fmt::format("Гос. цена — целое число от 0 до {}", MAX_PRICE)));
                showPriceEdit(*dev, businessId);
                return;
            }
            // Ре-валидация на клике: бизнес могли снести, пока висел диалог.
            if (!m_businessService.setPrice(businessId, static_cast<std::int64_t>(price)))
            {
                dev->sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} не найден", businessId)));
                return;
            }
            // Статус в лейбле зависит от цены («Продаётся» / «Не продаётся»); у
            // занятой точки цена на текст не влияет — refresh безопасен в обоих случаях.
            const BusinessService::Business *updated = m_businessService.getBusiness(businessId);
            refreshBusinessLabel(businessId, updated ? onlineNameOf(updated->owner) : std::string());
            dev->sendClientMessage(DEV_COLOUR,
                                   u(fmt::format("Бизнес #{}: гос. цена теперь ${}", businessId, price)));
            showDevList(*dev);
        });
}

void BusinessSystem::showDevRemove(IPlayer &player)
{
    const int playerId = player.getID();
    if (m_businessService.count() == 0)
    {
        player.sendClientMessage(DEV_COLOUR, u("Бизнесов нет"));
        return;
    }

    // Снимок id в порядке показа: listItem клиента адресует именно его.
    std::vector<int> ids;
    std::string body = "ID\tТип\tВладелец\n";
    for (const auto &[id, business] : m_businessService.businesses())
    {
        ids.push_back(id);
        body += fmt::format("{}\t{}\t{}\n", id, m_businessService.typeName(business.type),
                            business.owner.empty() ? "—" : business.owner);
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Снести бизнес", body, "Снести", "Назад"),
        [this, playerId, ids](DialogResponse response, int listItem, StringView)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showDevMenu(*dev);
                return;
            }
            if (listItem < 0 || listItem >= static_cast<int>(ids.size()))
            {
                return;
            }
            const int id = ids[static_cast<std::size_t>(listItem)];
            // Ставки — уже списанные деньги живых игроков: снос без возврата
            // просто сжёг бы их. Торги закрывает общий сервис, он же вернёт деньги.
            const int category = m_auctionService.categoryByKey(AUCTION_KEY);
            if (category >= 0)
            {
                m_auctionService.closeLot(static_cast<std::size_t>(category), id,
                                          fmt::format("Бизнес #{} снесён", id));
            }
            // Владение живёт в БД и через removeBusiness не проходит — строку
            // сносим отдельно, иначе она осиротеет и займёт аккаунт впустую.
            // Заказы снимаем ДО стирания записи: возврат считается по составу и
            // ценам этой точки, а после removeBusiness их уже не спросить.
            refundOrdersOf(id);
            eraseOwnershipRow(id);
            // Рантайм-точки снимаем ДО удаления записи: иначе пикапы остались бы
            // висеть без владельца.
            despawnBusiness(id);
            if (m_businessService.removeBusiness(id))
            {
                dev->sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} снесён", id)));
            }
        });
}

// ------------------------------------------------------------------ дев-склад

void BusinessSystem::showDevStockPick(IPlayer &player)
{
    const int playerId = player.getID();
    if (m_businessService.count() == 0)
    {
        player.sendClientMessage(DEV_COLOUR, u("Бизнесов нет"));
        return;
    }

    // Снимок id в порядке показа: listItem клиента адресует именно его (как в сносе).
    std::vector<int> ids;
    std::string body = "ID\tТип\tНа складе\n";
    for (const auto &[id, business] : m_businessService.businesses())
    {
        // Сумма по всем товарам типа: полный расклад даёт уже склад конкретной точки.
        int units = 0;
        for (const BusinessService::GoodDef &good : m_businessService.goods(business.type))
        {
            units += m_businessService.stockOf(id, good.itemType);
        }
        ids.push_back(id);
        body += fmt::format("{}\t{}\t{} ед.\n", id, m_businessService.typeName(business.type), units);
    }
    body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Склад точки — выбор", body, "Открыть", "Назад"),
                         [this, playerId, ids](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *dev = m_core.getPlayers().get(playerId);
                             if (!dev)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showDevMenu(*dev);
                                 return;
                             }
                             if (listItem < 0 || listItem >= static_cast<int>(ids.size()))
                             {
                                 return; // индекс от клиента — по снимку списка
                             }
                             showDevStock(*dev, ids[static_cast<std::size_t>(listItem)]);
                         });
}

void BusinessSystem::showDevStock(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    // Ре-валидация: точку могли снести, пока висел выбор.
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        player.sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} не найден", businessId)));
        return;
    }
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(business->type);
    if (goods.empty())
    {
        player.sendClientMessage(DEV_COLOUR, u("У этого типа нет ассортимента"));
        showDevStockPick(player);
        return;
    }

    // Строки товаров, последняя — очистка. Индексы: товар == listItem, очистка == размер.
    std::string body = "Товар\tНа складе\tЦена\n";
    for (const BusinessService::GoodDef &good : goods)
    {
        body += fmt::format("{}\t{}/{}\t{}\n", goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                            m_businessService.stockOf(businessId, good.itemType), good.stockCap,
                            Money::text(good.price));
    }
    body += "» Очистить склад (все товары в 0)";

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_TABLIST_HEADERS,
                   fmt::format("Склад #{} — {}", businessId, m_businessService.typeName(business->type)), body,
                   "Выбрать", "Назад"),
        [this, playerId, businessId, count = static_cast<int>(goods.size())](DialogResponse response, int listItem,
                                                                            StringView)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left || listItem < 0 || listItem > count)
            {
                showDevStockPick(*dev);
                return;
            }
            if (listItem == count) // последняя строка — очистка склада
            {
                showDevStockClear(*dev, businessId);
                return;
            }
            showDevStockAmount(*dev, businessId, static_cast<std::size_t>(listItem));
        });
}

void BusinessSystem::showDevStockAmount(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(business->type);
    if (goodIndex >= goods.size())
    {
        return;
    }
    const BusinessService::GoodDef &good = goods[goodIndex];
    // Потолок выдачи — свободное место: склад клампится в сервисе, и просить больше
    // бессмысленно (сервис молча срежет разницу).
    const int room = m_businessService.stockRoom(businessId, good.itemType);
    if (room <= 0)
    {
        player.sendClientMessage(DEV_COLOUR,
                                 u(fmt::format("Склад по «{}» полон — сперва очистите",
                                               goodDisplayName(good, m_inventoryService.itemName(good.itemType)))));
        showDevStock(player, businessId);
        return;
    }

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                   fmt::format("Бизнес #{}\nНа складе: {}/{}\nМожно выдать: {}\n\nСколько выдать? Товар ложится на "
                               "склад точки сразу и бесплатно.",
                               businessId, m_businessService.stockOf(businessId, good.itemType), good.stockCap, room),
                   "Выдать", "Назад"),
        [this, playerId, businessId, goodIndex, room](DialogResponse response, std::int64_t value)
        {
            IPlayer *dev = m_core.getPlayers().get(playerId);
            if (!dev)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showDevStock(*dev, businessId);
                return;
            }
            // Ре-валидация на клике: точку могли снести, ассортимент типа — сменить.
            const BusinessService::Business *current = m_businessService.getBusiness(businessId);
            if (!current)
            {
                dev->sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} не найден", businessId)));
                return;
            }
            const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(current->type);
            if (goodIndex >= goods.size())
            {
                return;
            }
            if (value <= 0 || value > room)
            {
                dev->sendClientMessage(DEV_COLOUR, u(fmt::format("Выдать можно от 1 до {}", room)));
                showDevStockAmount(*dev, businessId, goodIndex);
                return;
            }
            const int itemType = goods[goodIndex].itemType;
            const int added = m_businessService.addStock(businessId, itemType, static_cast<int>(value));
            dev->sendClientMessage(DEV_COLOUR,
                                   u(fmt::format("Бизнес #{}: выдано {} x{}, на складе {}/{}", businessId,
                                                 goodDisplayName(goods[goodIndex],
                                                                 m_inventoryService.itemName(itemType)),
                                                 added,
                                                 m_businessService.stockOf(businessId, itemType),
                                                 goods[goodIndex].stockCap)));
            showDevStock(*dev, businessId);
        });
}

void BusinessSystem::showDevStockClear(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    // Подтверждение, потому что стираем ОПЛАЧЕННЫЙ игроком товар: закупку ему никто не
    // вернёт, и промах по строке списка стоил бы ему денег.
    std::string body = fmt::format("Обнулить ВЕСЬ склад бизнеса #{} ({})?\n\nСейчас лежит:\n", businessId,
                                   m_businessService.typeName(business->type));
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        body += fmt::format("- {}: {}\n", goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                            m_businessService.stockOf(businessId, good.itemType));
    }
    body += "\nЭто товар, за который владелец заплатил. Возврата не будет.";

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Очистить склад", body, "Очистить", "Назад"),
                         [this, playerId, businessId](DialogResponse response, int, StringView)
                         {
                             IPlayer *dev = m_core.getPlayers().get(playerId);
                             if (!dev)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showDevStock(*dev, businessId);
                                 return;
                             }
                             clearDevStock(*dev, businessId);
                         });
}

void BusinessSystem::clearDevStock(IPlayer &player, int businessId)
{
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        player.sendClientMessage(DEV_COLOUR, u(fmt::format("Бизнес #{} не найден", businessId)));
        return;
    }

    // Списываем через consumeStock: он же уведомляет наблюдателя, и БД получает 0 тем
    // же путём, что при продаже. Обход ассортимента безопасен — список товаров живёт в
    // реестре ТИПА, а сервис в цикле правит только остатки конкретной точки.
    int cleared = 0;
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        const int stock = m_businessService.stockOf(businessId, good.itemType);
        if (stock > 0 && m_businessService.consumeStock(businessId, good.itemType, stock))
        {
            cleared += stock;
        }
    }
    player.sendClientMessage(DEV_COLOUR,
                             u(fmt::format("Бизнес #{}: склад очищен, снято {} ед.", businessId, cleared)));
    showDevStock(player, businessId);
}

// ------------------------------------------------------------------ рантайм точки

void BusinessSystem::spawnBusiness(const BusinessService::Business &business)
{
    if (m_runtime.count(business.id))
    {
        return; // уже заведён
    }
    if (!m_businessService.catalogValid(business.type, business.interiorIndex))
    {
        return; // битый индекс/незарегистрированный тип — точку не заводим
    }
    const BusinessService::CatalogEntry &entry =
        m_businessService.catalog(business.type)[static_cast<std::size_t>(business.interiorIndex)];

    Runtime runtime;
    runtime.enterPickup = m_pickupService.add(
        ENTRANCE_PICKUP_MODEL, PICKUP_TYPE, business.entrance,
        [this, id = business.id](IPlayer &player)
        {
            onEnterPickup(id, player);
        },
        0);
    runtime.mapIcon = m_mapIconService.addGlobal(BUSINESS_MAP_ICON, business.entrance, Colour::White(),
                                                 MapIconStyle_Global, MapIconService::RADAR_STREAM_DISTANCE);
    // Текст в основном мире (vw 0), у входа: тип, номер и статус. На спавне бизнес
    // ВСЕГДА ничейный (владение приходит из БД позже и перерисует лейбл), поэтому
    // имя владельца здесь не нужно.
    runtime.label = m_labelService.add(u(labelText(business, {})), business.entrance, BUSINESS_LABEL_COLOUR,
                                       BUSINESS_LABEL_DRAW_DISTANCE, BUSINESS_LABEL_TEST_LOS);

    // Пикап выхода — ЗА СПИНОЙ вошедшего: тот же приём, что и у выхода наружу (там
    // точка за спиной создателя). Угол округляем — пикап обязан лежать на оси.
    const Vector3 exitPickupPos =
        Geometry::backOf(entry.insideSpawn, Geometry::snapToQuarterTurn(entry.insideAngle), INSIDE_EXIT_DISTANCE);
    runtime.exitPickup = m_pickupService.add(
        EXIT_PICKUP_MODEL, PICKUP_TYPE, exitPickupPos,
        [this, id = business.id](IPlayer &player)
        {
            onExitPickup(id, player);
        },
        static_cast<std::uint32_t>(business.virtualWorld));

    m_runtime[business.id] = runtime;
}

void BusinessSystem::despawnBusiness(int businessId)
{
    const auto it = m_runtime.find(businessId);
    if (it == m_runtime.end())
    {
        return;
    }
    m_pickupService.remove(it->second.enterPickup);
    m_pickupService.remove(it->second.exitPickup);
    m_mapIconService.removeGlobal(it->second.mapIcon);
    m_labelService.remove(it->second.label);
    m_runtime.erase(it);
}

void BusinessSystem::onEnterPickup(int businessId, IPlayer &player)
{
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business || !m_businessService.catalogValid(business->type, business->interiorIndex))
    {
        return;
    }
    // Внутрь — только пешком: въезжать в интерьер на машине нельзя. Торги тоже
    // из машины не открываем — диалог отнимает управление у едущего.
    if (m_stateService.getState(player.getID()) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта"));
        return;
    }

    // Ничейный бизнес не работает, а продаётся государством: вместо интерьера —
    // предложение купить по госцене.
    if (business->owner.empty())
    {
        showPurchaseOffer(player, businessId);
        return;
    }

    const BusinessService::CatalogEntry &entry =
        m_businessService.catalog(business->type)[static_cast<std::size_t>(business->interiorIndex)];
    // С РАЗВОРОТОМ: угол интерьера замерен вместе с точкой спавна, без него игрок
    // появлялся бы лицом туда же, куда шёл снаружи. Округляем — угол каталога тоже
    // обязан лежать на оси.
    m_locationService.teleport(player, entry.insideSpawn, static_cast<unsigned>(entry.interiorId),
                               business->virtualWorld, Geometry::snapToQuarterTurn(entry.insideAngle));
    m_cameraService.setBehind(player); // иначе камера осталась бы смотреть «наружным» курсом

    // Меню бизнеса НЕ открываем: диалог поверх входа мешал бы, а витрину даёт
    // прилавок. Вместо него — попап с названием точки (английский, как все попапы
    // проекта) и звук входа в магазин.
    m_noticeService.show(player, m_businessService.typePopupName(business->type), ENTER_POPUP_TIME, INFO_COLOUR);
    m_audioService.playSound(player, ENTER_SOUND);
}

void BusinessSystem::onExitPickup(int businessId, IPlayer &player)
{
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    // Разворот — угол создателя + 180: точка выхода лежит ЗА спиной создателя, вход
    // остаётся позади вышедшего, и смотреть он должен ОТ входа (см. HouseSystem).
    m_locationService.teleport(player, business->exit, 0, 0,
                               Geometry::snapToQuarterTurn(business->exitAngle + 180.0f));
    m_cameraService.setBehind(player); // камера — за спину, вдоль нового направления
}

// ------------------------------------------------------------------ интерфейс «Бизнес»

std::string BusinessSystem::ownerKeyOf(int playerId) const
{
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return {};
    }
    return std::to_string(session->accountId);
}

void BusinessSystem::showBusinessMenu(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const bool isOwner = !business->owner.empty() && business->owner == ownerKeyOf(playerId);

    // Пункты видны ВСЕГДА (правило проекта): управление недоступно не владельцу —
    // объясняется по клику, а не прячется.
    const std::string body = fmt::format("{}\nУправление бизнесом\nИнформация", m_businessService.typeName(business->type));

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Бизнес", body, "Выбрать", "Закрыть"),
        [this, playerId, businessId, isOwner](DialogResponse response, int listItem, StringView)
        {
            IPlayer *visitor = m_core.getPlayers().get(playerId);
            if (!visitor || response != DialogResponse_Left)
            {
                return;
            }
            const BusinessService::Business *live = m_businessService.getBusiness(businessId);
            if (!live)
            {
                return; // бизнес снесли, пока висел диалог
            }
            switch (listItem)
            {
            case 0:
                // Геймплей типа: сервис зовёт обработчик, зарегистрированный системой
                // этого типа. Общий интерфейс про 24/7 ничего не знает.
                m_businessService.openVisitorMenu(live->type, *visitor, businessId);
                break;
            case 1:
                if (!isOwner)
                {
                    visitor->sendClientMessage(ERROR_COLOUR, u("Управление доступно только владельцу бизнеса"));
                    return;
                }
                showOwnerMenu(*visitor, businessId);
                break;
            case 2:
                visitor->sendClientMessage(
                    INFO_COLOUR, u(fmt::format("Бизнес #{}: {}. Владелец: {}", live->id,
                                               m_businessService.typeName(live->type),
                                               live->owner.empty() ? "свободен" : "есть")));
                break;
            default:
                break;
            }
        });
}

void BusinessSystem::showOwnerMenu(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }

    const std::string body = fmt::format("Забрать доход (${})\nЗакрыть", business->balance);
    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Управление бизнесом", body, "Выбрать", "Назад"),
                         [this, playerId, businessId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (!owner || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             if (listItem == 0)
                             {
                                 withdrawIncome(*owner, businessId);
                             }
                         });
}

void BusinessSystem::withdrawIncome(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    // Ре-валидация на клике: владение и копилка могли смениться, пока висел диалог.
    if (!business || business->owner.empty() || business->owner != ownerKeyOf(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это не ваш бизнес"));
        return;
    }
    const std::int64_t amount = m_businessService.withdrawBalance(businessId);
    if (amount <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Копилка пуста"));
        return;
    }
    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы забрали ${} из копилки бизнеса", amount)));
}

// ------------------------------------------------------------------ /business (владелец)

void BusinessSystem::showManageMenu(IPlayer &player)
{
    const int playerId = player.getID();
    const std::string ownerKey = ownerKeyOf(playerId);
    if (ownerKey.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Управление бизнесом доступно только под аккаунтом"));
        return;
    }
    if (!m_businessService.isOwnershipLoaded())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Данные о владении ещё грузятся — попробуйте через минуту"));
        return;
    }
    const std::vector<int> owned = m_businessService.businessesOf(ownerKey);
    if (owned.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас нет бизнеса. Свободные точки продаёт государство"));
        return;
    }
    const int businessId = owned.front(); // один бизнес на аккаунт (UNIQUE в БД)
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }

    // Пункты видны ВСЕГДА (правило проекта): недоступность объясняется по клику, а
    // не прячется. ПОРЯДОК СТРОК = порядок case-веток обработчика.
    // ПОРЯДОК СТРОК = порядок case-веток обработчика. Инвентаризация и заказ стоят
    // рядом сознательно: владелец смотрит остатки и тут же добирает товар.
    std::string body;
    body += "Информация\n";
    body += fmt::format("Прибыль (в копилке: ${})\n", business->balance);
    body += "Инвентаризация\n";
    body += "Заказать товар\n";
    body += "Улучшения\n";
    body += "Продать игроку\n";
    body += "Передать игроку\n";
    body += "Отказаться от бизнеса";

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_LIST, fmt::format("Бизнес #{} — {}", businessId, m_businessService.typeName(business->type)),
                   body, "Выбрать", "Закрыть"),
        [this, playerId, businessId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner || response != DialogResponse_Left)
            {
                return;
            }
            // Ре-валидация на клике: бизнес могли продать/снести, пока висел диалог.
            const BusinessService::Business *current = m_businessService.getBusiness(businessId);
            if (!current || current->owner != ownerKeyOf(playerId))
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Это уже не ваш бизнес"));
                return;
            }
            switch (listItem)
            {
            case 0:
                showBusinessInfo(*owner, businessId);
                break;
            case 1:
                showIncomeHistory(*owner, businessId);
                break;
            case 2:
                showInventory(*owner, businessId);
                break;
            case 3:
                showOrderMenu(*owner, businessId);
                break;
            case 4:
                owner->sendClientMessage(INFO_COLOUR, u("Улучшения бизнеса ещё в разработке"));
                showManageMenu(*owner);
                break;
            case 5:
                showSellInput(*owner, businessId);
                break;
            case 6:
                showTransferInput(*owner, businessId);
                break;
            case 7:
                showAbandonConfirm(*owner, businessId);
                break;
            default:
                break;
            }
        });
}

void BusinessSystem::showBusinessInfo(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }

    std::string body = fmt::format("Тип: {}\nНомер точки: #{}\n\n", m_businessService.typeName(business->type),
                                   businessId);
    body += fmt::format("В копилке сейчас: ${}\n", business->balance);
    body += fmt::format("Гос. оценка точки: ${}\n\n", business->price);
    body += "Как точка приносит деньги:\n";
    body += "- посетитель покупает товар у прилавка внутри,\n";
    body += "  и вся уплаченная сумма падает в копилку точки;\n";
    body += "- копилку вы забираете сами: «Прибыль» в этом меню\n";
    body += "  либо «Управление бизнесом» внутри точки.\n\n";
    body += "Что стоит знать:\n";
    body += "- товар на складе кончается: закупайте его через\n";
    body += "  «Заказать товар», иначе точка перестанет продавать;\n";
    body += "- покупать у себя можно, но невыгодно: деньги вернутся\n";
    body += "  в копилку, а закупка съеденной единицы уже оплачена;\n";
    body += "- копилка не капает сама по себе — нужны живые покупатели,\n";
    body += "  поэтому точка у оживлённого места выгоднее;\n";
    body += "- пока точка ваша, её могут купить только у вас: свободной\n";
    body += "  она станет, лишь если вы от неё откажетесь.";

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Информация о бизнесе", body, "Назад", ""),
                         [this, playerId](DialogResponse, int, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (owner)
                             {
                                 showManageMenu(*owner);
                             }
                         });
}

void BusinessSystem::persistIncomeDay(int businessId, std::int64_t income)
{
    if (income <= 0)
    {
        return;
    }
    // ОТНОСИТЕЛЬНАЯ запись (amount = amount + ?), поэтому ключ упорядочивания не
    // нужен: сложение коммутативно, и порядок задач в пуле безразличен. Дата берётся
    // БД (CURDATE), а не сервером — чтобы день не разъезжался с выборкой ниже.
    DatabaseManager::throwQuery(
        [businessId, income](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO business_income (business_id, day, amount) VALUES (?, CURDATE(), ?) "
                     "ON DUPLICATE KEY UPDATE amount = amount + VALUES(amount)")
                .bind(businessId, income)
                .execute();
        },
        [businessId](const std::string &error)
        {
            LogManager::log(Error,
                            fmt::format("BusinessSystem: не записана выручка бизнеса {}: {}", businessId, error));
        });
}

void BusinessSystem::showIncomeHistory(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session)
    {
        return;
    }

    using IncomeRow = std::pair<std::string, std::int64_t>; // день (как есть из БД), сумма
    DatabaseManager::selectQuery<std::vector<IncomeRow>>(
        [businessId](mysqlx::Schema schema)
        {
            // Последняя НЕДЕЛЯ включая сегодня. Границу считает БД — сервер и БД
            // могут стоять в разных часовых поясах, а день записи задаёт БД.
            mysqlx::SqlResult rows = schema.getSession()
                                         .sql("SELECT DATE_FORMAT(day, '%d.%m'), amount FROM business_income "
                                              "WHERE business_id = ? AND day >= CURDATE() - INTERVAL 6 DAY "
                                              "ORDER BY day DESC")
                                         .bind(businessId)
                                         .execute();
            std::vector<IncomeRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                try
                {
                    result.emplace_back(row.get(0).get<std::string>(), row.get(1).get<std::int64_t>());
                }
                catch (...)
                {
                    continue; // порченая строка теряет только себя
                }
            }
            return result;
        },
        [this, playerId, serial = session->serial, businessId](std::vector<IncomeRow> days)
        {
            // Serial-guard: за время запроса в слоте мог оказаться другой игрок.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }

            std::int64_t total = 0;
            std::string body = "Дата\tВыручка\n";
            for (const auto &[day, amount] : days)
            {
                total += amount;
                body += fmt::format("{}\t${}\n", day, amount);
            }
            if (days.empty())
            {
                body += "за неделю\tпусто\n";
            }
            body += fmt::format("ВСЕГО за неделю\t${}", total);

            m_dialogService.show(*owner,
                                 makeDialog(DialogStyle_TABLIST_HEADERS, "Прибыль за неделю", body, "Назад", ""),
                                 [this, playerId](DialogResponse, int, StringView)
                                 {
                                     IPlayer *back = m_core.getPlayers().get(playerId);
                                     if (back)
                                     {
                                         showManageMenu(*back);
                                     }
                                 });
        },
        [this, playerId](const std::string &error)
        {
            LogManager::log(Error, "BusinessSystem: не удалось прочитать выручку: " + error);
            if (IPlayer *owner = m_core.getPlayers().get(playerId))
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Не удалось получить историю выручки, попробуйте позже"));
            }
        });
}

bool BusinessSystem::handoverTargetValid(IPlayer &owner, int targetId, std::uint32_t targetSerial,
                                         std::string &reason) const
{
    if (targetId == owner.getID())
    {
        reason = "Нельзя передать бизнес самому себе";
        return false;
    }
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target)
    {
        reason = "Такого игрока нет в сети";
        return false;
    }
    const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
    if (!targetSession)
    {
        reason = "Этот игрок ещё не вошёл в аккаунт";
        return false;
    }
    // Слот id мог занять ДРУГОЙ игрок, пока висел диалог: сделка идёт только с тем
    // аккаунтом, которому её и предлагали. 0 — первичная проверка, серила ещё нет.
    if (targetSerial != 0 && targetSession->serial != targetSerial)
    {
        reason = "Этот игрок сменился — начните заново";
        return false;
    }
    if (m_businessService.ownsBusiness(std::to_string(targetSession->accountId)))
    {
        reason = "У этого игрока уже есть свой бизнес";
        return false;
    }
    // Позиции ПРИНЯТЫЕ сервером: «я рядом» подделать нельзя.
    const Vector3 here = m_locationService.getPosition(owner.getID());
    const Vector3 there = m_locationService.getPosition(targetId);
    const float dx = here.x - there.x;
    const float dy = here.y - there.y;
    const float dz = here.z - there.z;
    if (dx * dx + dy * dy + dz * dz > HANDOVER_RADIUS * HANDOVER_RADIUS)
    {
        reason = "Игрок должен стоять рядом с вами";
        return false;
    }
    return true;
}

void BusinessSystem::showSellInput(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Продать бизнес",
                   fmt::format("Введите id покупателя.\nОн должен стоять рядом (не дальше {:.0f} м) и не иметь "
                               "своего бизнеса.",
                               HANDOVER_RADIUS),
                   "Далее", "Назад"),
        [this, playerId, businessId](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showManageMenu(*owner);
                return;
            }
            const int targetId = static_cast<int>(value);
            std::string reason;
            if (value < 0 || value >= MAX_PLAYERS || !handoverTargetValid(*owner, targetId, 0, reason))
            {
                owner->sendClientMessage(ERROR_COLOUR, u(reason.empty() ? "Игрок не найден" : reason));
                showManageMenu(*owner);
                return;
            }
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            showSellPriceInput(*owner, businessId, targetId, targetSession->serial);
        });
}

void BusinessSystem::showSellPriceInput(IPlayer &player, int businessId, int targetId, std::uint32_t targetSerial)
{
    const int playerId = player.getID();
    IPlayer *target = m_core.getPlayers().get(targetId);
    if (!target)
    {
        return;
    }
    const std::string targetName = Encoding::neutralizeColorCodes(std::string_view(target->getName().to_string()));

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Продать бизнес",
                   fmt::format("Покупатель: {}[{}]\n\nЗа сколько продаёте?\nОт 1 до {}.", targetName, targetId,
                               MAX_PRICE),
                   "Предложить", "Назад"),
        [this, playerId, businessId, targetId, targetSerial](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showManageMenu(*owner);
                return;
            }
            if (value < 1 || value > MAX_PRICE)
            {
                owner->sendClientMessage(ERROR_COLOUR, u(fmt::format("Цена — целое от 1 до {}", MAX_PRICE)));
                showSellPriceInput(*owner, businessId, targetId, targetSerial);
                return;
            }
            offerToTarget(*owner, businessId, targetId, targetSerial, value);
        });
}

void BusinessSystem::showTransferInput(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Передать бизнес",
                   fmt::format("Введите id получателя.\nОн должен стоять рядом (не дальше {:.0f} м) и не иметь "
                               "своего бизнеса.\n\nПередача БЕСПЛАТНАЯ — денег с него не возьмут.",
                               HANDOVER_RADIUS),
                   "Предложить", "Назад"),
        [this, playerId, businessId](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showManageMenu(*owner);
                return;
            }
            const int targetId = static_cast<int>(value);
            std::string reason;
            if (value < 0 || value >= MAX_PLAYERS || !handoverTargetValid(*owner, targetId, 0, reason))
            {
                owner->sendClientMessage(ERROR_COLOUR, u(reason.empty() ? "Игрок не найден" : reason));
                showManageMenu(*owner);
                return;
            }
            const PlayerSessionService::Session *targetSession = m_sessionService.get(targetId);
            offerToTarget(*owner, businessId, targetId, targetSession->serial, 0);
        });
}

void BusinessSystem::offerToTarget(IPlayer &seller, int businessId, int targetId, std::uint32_t targetSerial,
                                   std::int64_t price)
{
    IPlayer *target = m_core.getPlayers().get(targetId);
    const PlayerSessionService::Session *sellerSession = m_sessionService.get(seller.getID());
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!target || !sellerSession || !business)
    {
        return;
    }

    const std::string sellerName = Encoding::neutralizeColorCodes(std::string_view(seller.getName().to_string()));
    const std::string what = fmt::format("{} (точка #{})", m_businessService.typeName(business->type), businessId);
    const std::string body =
        price > 0 ? fmt::format("{} продаёт вам {}\nза {}.\n\nПринять предложение?", sellerName, what,
                                Money::text(price))
                  : fmt::format("{} передаёт вам {}\nбесплатно.\n\nПринять?", sellerName, what);

    seller.sendClientMessage(INFO_COLOUR, u("Предложение отправлено, ждём ответа"));

    // Согласие обрабатываем от лица ПОЛУЧАТЕЛЯ: сделку завершает он, и все проверки
    // на его стороне повторяются заново — диалог мог провисеть сколько угодно.
    m_dialogService.show(*target, makeDialog(DialogStyle_MSGBOX, "Предложение", body, "Принять", "Отказаться"),
                         [this, targetId, targetSerial, businessId, sellerAccount = sellerSession->accountId,
                          price](DialogResponse response, int, StringView)
                         {
                             IPlayer *buyer = m_core.getPlayers().get(targetId);
                             if (!buyer)
                             {
                                 return;
                             }
                             const PlayerSessionService::Session *current = m_sessionService.get(targetId);
                             if (!current || current->serial != targetSerial)
                             {
                                 return; // в слоте уже другой игрок — сделка недействительна
                             }
                             if (response != DialogResponse_Left)
                             {
                                 buyer->sendClientMessage(INFO_COLOUR, u("Вы отказались от предложения"));
                                 if (const int sellerId = m_sessionService.playerByAccount(sellerAccount);
                                     sellerId >= 0)
                                 {
                                     if (IPlayer *seller = m_core.getPlayers().get(sellerId))
                                     {
                                         seller->sendClientMessage(ERROR_COLOUR, u("Ваше предложение отклонили"));
                                     }
                                 }
                                 return;
                             }
                             completeHandover(*buyer, businessId, sellerAccount, price);
                         });
}

void BusinessSystem::completeHandover(IPlayer &target, int businessId,
                                      PlayerSessionService::AccountId sellerAccount, std::int64_t price)
{
    const int targetId = target.getID();
    // Продавец обязан быть онлайн: деньги отдаём наличными, офлайну вручить некому.
    const int sellerId = m_sessionService.playerByAccount(sellerAccount);
    IPlayer *seller = sellerId >= 0 ? m_core.getPlayers().get(sellerId) : nullptr;
    if (!seller)
    {
        target.sendClientMessage(ERROR_COLOUR, u("Продавец вышел из игры — сделка отменена"));
        return;
    }

    // Владение могло смениться, пока висело согласие.
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business || business->owner != std::to_string(sellerAccount))
    {
        target.sendClientMessage(ERROR_COLOUR, u("Этот бизнес уже не принадлежит продавцу"));
        return;
    }
    // Те же проверки, что и на старте: рядом, без своего бизнеса, не сам себе.
    std::string reason;
    if (!handoverTargetValid(*seller, targetId, 0, reason))
    {
        target.sendClientMessage(ERROR_COLOUR, u(reason));
        seller->sendClientMessage(ERROR_COLOUR, u(reason));
        return;
    }

    const std::string targetKey = std::to_string(m_sessionService.getAccountId(targetId));
    if (price > 0)
    {
        // Деньги забираем ДО передачи владения: обратный порядок отдал бы точку
        // без оплаты, если денег не хватило.
        if (!m_moneyService.take(target, static_cast<unsigned long long>(price)))
        {
            target.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(price))));
            seller->sendClientMessage(ERROR_COLOUR, u("У покупателя не хватило денег"));
            return;
        }
    }
    if (!m_businessService.setOwner(businessId, targetKey))
    {
        if (price > 0)
        {
            m_moneyService.giveMoney(target, static_cast<unsigned long long>(price));
        }
        target.sendClientMessage(ERROR_COLOUR, u("Бизнес исчез — сделка отменена"));
        return;
    }
    if (price > 0)
    {
        m_moneyService.giveMoney(*seller, static_cast<unsigned long long>(price));
    }

    const std::string targetName = Encoding::neutralizeColorCodes(std::string_view(target.getName().to_string()));
    const std::string sellerName = Encoding::neutralizeColorCodes(std::string_view(seller->getName().to_string()));
    if (price > 0)
    {
        seller->sendClientMessage(INFO_COLOUR, u(fmt::format("Бизнес продан {} за {}", targetName, Money::text(price))));
        target.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы купили бизнес у {} за {}", sellerName, Money::text(price))));
    }
    else
    {
        seller->sendClientMessage(INFO_COLOUR, u(fmt::format("Бизнес передан {}", targetName)));
        target.sendClientMessage(INFO_COLOUR, u(fmt::format("{} передал вам бизнес", sellerName)));
    }
}

void BusinessSystem::showAbandonConfirm(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const std::string body =
        fmt::format("Отказаться от точки «{}» #{}?\n\nОна вернётся государству и снова будет продаваться "
                    "по госцене {}.\nДЕНЕГ ЗА ЭТО НЕ ДАЮТ. Копилку (${}) заберите заранее — вместе с точкой "
                    "она пропадёт.\n\nЭто действие нельзя отменить.",
                    m_businessService.typeName(business->type), businessId, Money::text(business->price),
                    business->balance);

    m_dialogService.show(
        player, makeDialog(DialogStyle_MSGBOX, "Отказ от бизнеса", body, "Отказаться", "Назад"),
        [this, playerId, businessId](DialogResponse response, int, StringView)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showManageMenu(*owner);
                return;
            }
            // Ре-валидация: владение могло смениться, пока висело подтверждение.
            const BusinessService::Business *business = m_businessService.getBusiness(businessId);
            if (!business || business->owner != ownerKeyOf(playerId))
            {
                owner->sendClientMessage(ERROR_COLOUR, u("Это уже не ваш бизнес"));
                return;
            }
            // Ничейная точка не работает и внутрь не пускает — заказ в неё доставить
            // было бы нельзя ни развозчику, ни новому владельцу. Снимаем заказы ДО
            // смены владельца: возврат за невывезенную часть уходит тому, кто платил.
            refundOrdersOf(businessId);
            m_businessService.setOwner(businessId, ""); // персист и лейбл доводит onOwnerChanged
            owner->sendClientMessage(INFO_COLOUR, u("Вы отказались от бизнеса — точка вернулась государству"));
        });
}

// ------------------------------------------------------------------ склад

void BusinessSystem::persistStock(int businessId, int itemType, int quantity)
{
    // АБСОЛЮТНАЯ запись под ключом упорядочивания. Относительная дельта тут не
    // годится: у остатка есть ПОТОЛОК, клампится он в памяти, и потерянная дельта
    // увела бы БД за границу. Ключ на (бизнес, товар) даёт порядок двух записей по
    // одной строке — продажа и доставка легко случаются подряд.
    DatabaseManager::throwQueryOrdered(
        fmt::format("business_stock:{}:{}", businessId, itemType),
        [businessId, itemType, quantity](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO business_stock (business_id, item_type, quantity) VALUES (?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE quantity = VALUES(quantity)")
                .bind(businessId, itemType, quantity)
                .execute();
        },
        [businessId, itemType](const std::string &error)
        {
            LogManager::log(Error, fmt::format("BusinessSystem: не записан остаток {}/{}: {}", businessId, itemType,
                                               error));
        });
}

void BusinessSystem::loadStockAsync()
{
    using StockRow = std::tuple<int, int, int>; // бизнес, товар, остаток
    DatabaseManager::selectQuery<std::vector<StockRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows =
                schema.getTable("business_stock").select("business_id", "item_type", "quantity").execute();
            std::vector<StockRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                try
                {
                    result.emplace_back(row.get(0).get<int>(), row.get(1).get<int>(), row.get(2).get<int>());
                }
                catch (...)
                {
                    continue; // порченая строка теряет только себя
                }
            }
            return result;
        },
        [this](std::vector<StockRow> rows)
        {
            for (const auto &[businessId, itemType, quantity] : rows)
            {
                // loadStock сам отбросит осиротевшие строки (точки нет) и товары,
                // выпавшие из ассортимента типа.
                m_businessService.loadStock(businessId, itemType, quantity);
            }
        },
        [](const std::string &error)
        {
            // Флаг готовности складу не нужен: пустой склад просто ничего не продаёт,
            // а владелец дозакажет. Ничего необратимого без этих данных не делается.
            LogManager::log(Error, "BusinessSystem: не удалось загрузить склады: " + error);
        });
}

std::unordered_map<int, int> &BusinessSystem::orderDraft(int playerId)
{
    return m_orderDrafts[playerId];
}

void BusinessSystem::clearOrderDraft(int playerId)
{
    m_orderDrafts.erase(playerId);
}

void BusinessSystem::showInventory(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }

    std::string body = "Товар\tНа складе\n";
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        body += fmt::format("{}\t{}/{}\n", goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                            m_businessService.stockOf(businessId, good.itemType), good.stockCap);
    }
    body.pop_back();

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Инвентаризация", body, "Назад", ""),
                         [this, playerId](DialogResponse, int, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (owner)
                             {
                                 showManageMenu(*owner);
                             }
                         });
}

std::string BusinessSystem::orderRowStatus(int businessId) const
{
    // Заказ у точки один (сделать второй не даёт showOrderConfirm), поэтому берём
    // первый: ordersOf отдаёт и лежащий в пуле, и тот, что уже везут.
    const std::vector<const BusinessOrderService::Order *> orders = m_orderService.ordersOf(businessId);
    if (orders.empty())
    {
        return "заказа нет";
    }
    const BusinessOrderService::Order *order = orders.front();
    if (order->driverId >= 0)
    {
        // Везут — отмена уже невозможна, и владелец видит это до клика.
        return fmt::format("заказ везут, {}/{}", order->boxesDone, BusinessOrderService::BOXES_PER_ORDER);
    }
    if (order->createdAt <= 0)
    {
        return "заказ ждёт развозчика"; // строка БД старого формата, без времени
    }
    return fmt::format("заказ от {}", TimeFormat::dateTime(order->createdAt));
}

void BusinessSystem::showOrderMenu(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    // ПОРЯДОК СТРОК = порядок case-веток обработчика. Отмена — оранжевая: это
    // единственный пункт меню, который отменяет уже оплаченное действие.
    const std::string body =
        fmt::format("Выбрать к заказу\nСделать заказ\n{{FFB400}}Отменить заказ ({})", orderRowStatus(businessId));

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Заказ товара", body, "Выбрать", "Назад"),
                         [this, playerId, businessId](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (!owner)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showManageMenu(*owner);
                                 return;
                             }
                             switch (listItem)
                             {
                             case 0:
                                 showOrderPicker(*owner, businessId);
                                 break;
                             case 1:
                                 showOrderConfirm(*owner, businessId);
                                 break;
                             case 2:
                                 cancelOrder(*owner, businessId);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void BusinessSystem::showOrderPicker(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const std::unordered_map<int, int> &draft = orderDraft(playerId);

    std::string body = "Товар\tНа складе\tЗаказано\n";
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        const auto ordered = draft.find(good.itemType);
        body += fmt::format("{}\t{}/{}\t{}\n", goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                            m_businessService.stockOf(businessId, good.itemType), good.stockCap,
                            ordered == draft.end() ? 0 : ordered->second);
    }
    body.pop_back();

    m_dialogService.show(
        player, makeDialog(DialogStyle_TABLIST_HEADERS, "Выбрать к заказу", body, "Изменить", "Назад"),
        [this, playerId, businessId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showOrderMenu(*owner, businessId);
                return;
            }
            const BusinessService::Business *current = m_businessService.getBusiness(businessId);
            if (!current || listItem < 0 ||
                listItem >= static_cast<int>(m_businessService.goods(current->type).size()))
            {
                return; // индекс от клиента — по живому ассортименту
            }
            showOrderAmountInput(*owner, businessId, static_cast<std::size_t>(listItem));
        });
}

void BusinessSystem::showOrderAmountInput(IPlayer &player, int businessId, std::size_t goodIndex)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(business->type);
    if (goodIndex >= goods.size())
    {
        return;
    }
    const BusinessService::GoodDef &good = goods[goodIndex];
    // Потолок заказа — СВОБОДНОЕ МЕСТО на складе: 100 всего и 99 лежит -> можно 1.
    const int room = m_businessService.stockRoom(businessId, good.itemType);

    if (room <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(fmt::format("Склад по «{}» полон", goodDisplayName(good, m_inventoryService.itemName(good.itemType)))));
        showOrderPicker(player, businessId);
        return;
    }

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, goodDisplayName(good, m_inventoryService.itemName(good.itemType)),
                   fmt::format("На складе: {}/{}\nМожно дозаказать: {}\nЦена закупки: {} за штуку\n\n"
                               "Сколько заказать? 0 — убрать из заказа.",
                               m_businessService.stockOf(businessId, good.itemType), good.stockCap, room,
                               Money::text(good.price * ORDER_PRICE_PERCENT / 100)),
                   "Сохранить", "Назад"),
        [this, playerId, businessId, goodIndex, room](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showOrderPicker(*owner, businessId);
                return;
            }
            const BusinessService::Business *current = m_businessService.getBusiness(businessId);
            if (!current)
            {
                return;
            }
            const std::vector<BusinessService::GoodDef> &goods = m_businessService.goods(current->type);
            if (goodIndex >= goods.size())
            {
                return;
            }
            if (value < 0 || value > room)
            {
                owner->sendClientMessage(ERROR_COLOUR,
                                         u(fmt::format("Можно заказать от 0 до {}", room)));
                showOrderAmountInput(*owner, businessId, goodIndex);
                return;
            }
            const int itemType = goods[goodIndex].itemType;
            if (value == 0)
            {
                orderDraft(playerId).erase(itemType); // 0 — снять товар с заказа
            }
            else
            {
                orderDraft(playerId)[itemType] = static_cast<int>(value);
            }
            showOrderPicker(*owner, businessId);
        });
}

void BusinessSystem::showOrderConfirm(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    const std::unordered_map<int, int> &draft = orderDraft(playerId);
    if (draft.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заказ пуст — сперва выберите товары"));
        showOrderMenu(player, businessId);
        return;
    }

    std::int64_t total = 0;
    std::string body;
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        const auto ordered = draft.find(good.itemType);
        if (ordered == draft.end() || ordered->second <= 0)
        {
            continue;
        }
        const std::int64_t unit = good.price * ORDER_PRICE_PERCENT / 100;
        const std::int64_t cost = unit * ordered->second;
        total += cost;
        body += fmt::format("{} — {} шт. по {} = {}\n", goodDisplayName(good, m_inventoryService.itemName(good.itemType)), ordered->second,
                            Money::text(unit), Money::text(cost));
    }
    body += fmt::format("\nИТОГО закупка: {}\n\nЗакупка стоит {}% от цены продажи.\nДальше — премия развозчикам.",
                        Money::text(total), ORDER_PRICE_PERCENT);

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Подтверждение заказа", body, "Далее", "Назад"),
                         [this, playerId, businessId, total](DialogResponse response, int, StringView)
                         {
                             IPlayer *owner = m_core.getPlayers().get(playerId);
                             if (!owner)
                             {
                                 return;
                             }
                             if (response != DialogResponse_Left)
                             {
                                 showOrderMenu(*owner, businessId);
                                 return;
                             }
                             showBonusInput(*owner, businessId, total);
                         });
}

std::int64_t BusinessSystem::planOrder(int playerId, int businessId, std::vector<std::pair<int, int>> &plan) const
{
    plan.clear();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return 0;
    }
    const auto draftIt = m_orderDrafts.find(playerId);
    if (draftIt == m_orderDrafts.end())
    {
        return 0;
    }
    // КЛАМПИМ по фактическому свободному месту: за время диалогов склад мог
    // измениться продажами, и платить за то, что не влезет, владелец не должен.
    std::int64_t total = 0;
    for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
    {
        const auto ordered = draftIt->second.find(good.itemType);
        if (ordered == draftIt->second.end() || ordered->second <= 0)
        {
            continue;
        }
        const int amount = std::min(ordered->second, m_businessService.stockRoom(businessId, good.itemType));
        if (amount <= 0)
        {
            continue;
        }
        plan.emplace_back(good.itemType, amount);
        total += good.price * ORDER_PRICE_PERCENT / 100 * amount;
    }
    return total;
}

void BusinessSystem::showBonusInput(IPlayer &player, int businessId, std::int64_t cost)
{
    const int playerId = player.getID();
    // Потолок премии — доля от закупки. Без него владелец вбил бы любую сумму и
    // перекачал деньги напарнику-развозчику мимо всякой экономики.
    const std::int64_t maxBonus = cost * BusinessOrderService::MAX_BONUS_PERCENT / 100;

    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Премия развозчикам",
                   fmt::format("Закупка: {}\n\nПремию получат те, кто ПРИВЕЗЁТ заказ.\nЧем она больше, тем "
                               "охотнее возьмут: список\nу развозчиков отсортирован по премии.\n\n"
                               "Максимум — {}% от закупки, то есть {}.\n0 — без премии.",
                               Money::text(cost), BusinessOrderService::MAX_BONUS_PERCENT, Money::text(maxBonus)),
                   "Заказать", "Назад"),
        [this, playerId, businessId, cost, maxBonus](DialogResponse response, std::int64_t value)
        {
            IPlayer *owner = m_core.getPlayers().get(playerId);
            if (!owner)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showOrderMenu(*owner, businessId);
                return;
            }
            if (value < 0 || value > maxBonus)
            {
                owner->sendClientMessage(ERROR_COLOUR,
                                         u(fmt::format("Премия — от 0 до {}", Money::text(maxBonus))));
                showBonusInput(*owner, businessId, cost);
                return;
            }
            placeOrder(*owner, businessId, value);
        });
}

void BusinessSystem::placeOrder(IPlayer &player, int businessId, std::int64_t bonus)
{
    const int playerId = player.getID();
    // Ре-валидация на клике: точку могли продать/снести, склад — набить продажами,
    // пока висели диалоги.
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business || business->owner != ownerKeyOf(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это уже не ваш бизнес"));
        clearOrderDraft(playerId);
        return;
    }
    // ОДИН незавершённый заказ на точку. Два заказа вместе могли бы превысить
    // потолок склада, и addStock молча срезал бы разницу — владелец заплатил бы за
    // товар, который некуда положить.
    if (m_orderService.hasActiveOrder(businessId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("По этой точке уже есть заказ — дождитесь доставки"));
        showManageMenu(player);
        return;
    }

    std::vector<std::pair<int, int>> plan;
    const std::int64_t cost = planOrder(playerId, businessId, plan);
    if (plan.empty() || cost <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Заказывать нечего — склад уже полон"));
        clearOrderDraft(playerId);
        showManageMenu(player);
        return;
    }
    // Премию пересчитываем от АКТУАЛЬНОЙ закупки: состав мог схлопнуться, пока шёл
    // ввод, и старый потолок стал бы завышенным.
    const std::int64_t maxBonus = cost * BusinessOrderService::MAX_BONUS_PERCENT / 100;
    const std::int64_t finalBonus = std::clamp<std::int64_t>(bonus, 0, maxBonus);
    const std::int64_t total = cost + finalBonus;

    // Деньги забираем ДО создания заказа: обратный порядок отдал бы заказ бесплатно.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(total)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(total))));
        showOrderMenu(player, businessId);
        return;
    }

    std::vector<BusinessOrderService::Item> items;
    items.reserve(plan.size());
    for (const auto &[itemType, amount] : plan)
    {
        items.push_back(BusinessOrderService::Item{itemType, amount});
    }
    const int orderId = m_orderService.create(businessId, std::move(items), finalBonus);
    if (orderId <= 0)
    {
        // Создать не вышло — деньги немедленно назад, товара не будет.
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(total));
        player.sendClientMessage(ERROR_COLOUR, u("Не удалось оформить заказ, деньги возвращены"));
        return;
    }
    clearOrderDraft(playerId);

    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Заказ оформлен: закупка {}, премия {}", Money::text(cost),
                                           Money::text(finalBonus))));
    player.sendClientMessage(INFO_COLOUR, u("Товар привезут развозчики — заказ уже в их списке"));
    showManageMenu(player);
}

void BusinessSystem::persistOrder(int orderId)
{
    const BusinessOrderService::Order *order = m_orderService.get(orderId);
    if (!order)
    {
        // Заказа больше нет (доставлен, отменён, точку снесли) — стираем строку.
        DatabaseManager::throwQueryOrdered(
            fmt::format("business_order:{}", orderId),
            [orderId](mysqlx::Schema schema)
            { schema.getTable("business_order").remove().where("id = :id").bind("id", orderId).execute(); },
            [orderId](const std::string &error)
            { LogManager::log(Error, fmt::format("BusinessSystem: не снят заказ {}: {}", orderId, error)); });
        return;
    }

    nlohmann::json items = nlohmann::json::array();
    for (const BusinessOrderService::Item &item : order->items)
    {
        items.push_back(nlohmann::json{{"item", item.itemType}, {"qty", item.quantity}});
    }
    // Ключ упорядочивания по заказу: создание, прогресс коробок и снятие спорят за
    // одну строку, а порядок задач в пуле не определён.
    DatabaseManager::throwQueryOrdered(
        fmt::format("business_order:{}", orderId),
        [orderId, businessId = order->businessId, bonus = order->bonus, boxesDone = order->boxesDone,
         payload = items.dump(), now = TimeFormat::nowUnix()](mysqlx::Schema schema)
        {
            schema.getSession()
                .sql("INSERT INTO business_order (id, business_id, bonus, boxes_done, items, created_at) "
                     "VALUES (?, ?, ?, ?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE boxes_done = VALUES(boxes_done), bonus = VALUES(bonus)")
                .bind(orderId, businessId, bonus, boxesDone, payload, now)
                .execute();
        },
        [orderId](const std::string &error)
        { LogManager::log(Error, fmt::format("BusinessSystem: не записан заказ {}: {}", orderId, error)); });
}

void BusinessSystem::loadOrdersAsync()
{
    using OrderRow = std::tuple<int, int, std::int64_t, int, std::string, std::int64_t>;
    DatabaseManager::selectQuery<std::vector<OrderRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("business_order")
                                         .select("id", "business_id", "bonus", "boxes_done", "items", "created_at")
                                         .execute();
            std::vector<OrderRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                try
                {
                    result.emplace_back(row.get(0).get<int>(), row.get(1).get<int>(), row.get(2).get<std::int64_t>(),
                                        row.get(3).get<int>(), row.get(4).get<std::string>(),
                                        row.get(5).get<std::int64_t>());
                }
                catch (...)
                {
                    continue; // порченая строка теряет только себя
                }
            }
            return result;
        },
        [this](std::vector<OrderRow> rows)
        {
            for (const auto &[id, businessId, bonus, boxesDone, payload, createdAt] : rows)
            {
                BusinessOrderService::Order order;
                order.id = id;
                order.businessId = businessId;
                order.bonus = bonus;
                order.boxesDone = boxesDone;
                order.createdAt = createdAt;
                // Состав — json; битый разбор теряет ТОЛЬКО этот заказ, а не все.
                try
                {
                    const nlohmann::json items = nlohmann::json::parse(payload);
                    if (!items.is_array())
                    {
                        continue;
                    }
                    for (const nlohmann::json &item : items)
                    {
                        if (!item.is_object() || !item.contains("item") || !item.contains("qty"))
                        {
                            continue;
                        }
                        order.items.push_back(
                            BusinessOrderService::Item{item["item"].get<int>(), item["qty"].get<int>()});
                    }
                }
                catch (...)
                {
                    LogManager::log(Warning, fmt::format("BusinessSystem: заказ {} с битым составом пропущен", id));
                    continue;
                }
                m_orderService.load(std::move(order));
            }
        },
        [](const std::string &error)
        { LogManager::log(Error, "BusinessSystem: не удалось загрузить заказы: " + error); });
}

std::int64_t BusinessSystem::orderCost(const BusinessOrderService::Order &order) const
{
    const BusinessService::Business *business = m_businessService.getBusiness(order.businessId);
    if (!business)
    {
        return 0;
    }
    std::int64_t cost = 0;
    for (const BusinessOrderService::Item &item : order.items)
    {
        for (const BusinessService::GoodDef &good : m_businessService.goods(business->type))
        {
            if (good.itemType == item.itemType)
            {
                cost += good.price * ORDER_PRICE_PERCENT / 100 * item.quantity;
                break;
            }
        }
    }
    return cost;
}

void BusinessSystem::cancelOrder(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business || business->owner != ownerKeyOf(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Это уже не ваш бизнес"));
        return;
    }

    // Ищем ИМЕННО свободный заказ: взятый развозчиком отменить нельзя — он уже
    // едет, а часть коробок могла быть выгружена.
    const BusinessOrderService::Order *target = nullptr;
    for (const BusinessOrderService::Order *order : m_orderService.pool())
    {
        if (order->businessId == businessId)
        {
            target = order;
            break;
        }
    }
    if (!target)
    {
        player.sendClientMessage(ERROR_COLOUR,
                                 u(m_orderService.hasActiveOrder(businessId)
                                       ? "Заказ уже везут — отменить нельзя"
                                       : "У этой точки нет заказа"));
        showOrderMenu(player, businessId);
        return;
    }

    // Возврат считаем по НЕВЫВЕЗЕННОЙ части: коробки, которые уже довезли, лежат на
    // складе, и деньги за них владелец получил товаром.
    const int boxesLeft = m_orderService.boxesLeft(target->id);
    const std::int64_t cost = orderCost(*target);
    const std::int64_t refund =
        (cost + target->bonus) * boxesLeft / BusinessOrderService::BOXES_PER_ORDER;
    m_orderService.remove(target->id);

    if (refund > 0)
    {
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(refund));
    }
    player.sendClientMessage(INFO_COLOUR,
                             u(fmt::format("Заказ отменён, возвращено {}", Money::text(refund))));
    showManageMenu(player);
}

void BusinessSystem::refundOrdersOf(int businessId)
{
    // Точка перестаёт работать (снос девом либо отказ владельца) — везти в неё нечего.
    // Снимаем ВСЕ её заказы, включая те, что уже везут: заказ в доставке иначе остался
    // бы висеть на несуществующей точке, и развозчик приехал бы в пустоту.
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    const std::string ownerKey = business ? business->owner : std::string();

    for (const BusinessOrderService::Order *order : m_orderService.ordersOf(businessId))
    {
        const int orderId = order->id;
        // Возврат — по НЕВЫВЕЗЕННОЙ части, как при отмене заказа владельцем: довезённые
        // коробки владелец уже получил товаром, а склад точки при сносе не компенсируют
        // (иначе за одно и то же платили бы дважды).
        const int boxesLeft = m_orderService.boxesLeft(orderId);
        const std::int64_t refund =
            (orderCost(*order) + order->bonus) * boxesLeft / BusinessOrderService::BOXES_PER_ORDER;
        m_orderService.remove(orderId); // после этого order висячий — им больше не пользуемся

        // Наличные сессионные: офлайну вручить некому. Снос точки — редкое дев-
        // действие, поэтому просто пишем в лог, а не заводим долговую очередь.
        const int ownerPlayerId = ownerKey.empty()
                                      ? -1
                                      : m_sessionService.playerByAccount(
                                            static_cast<PlayerSessionService::AccountId>(std::stoll(ownerKey)));
        IPlayer *owner = ownerPlayerId >= 0 ? m_core.getPlayers().get(ownerPlayerId) : nullptr;
        if (owner && refund > 0)
        {
            m_moneyService.giveMoney(*owner, static_cast<unsigned long long>(refund));
            owner->sendClientMessage(INFO_COLOUR,
                                     u(fmt::format("Точка больше не работает: за снятый заказ возвращено {}",
                                                   Money::text(refund))));
        }
        else if (refund > 0)
        {
            LogManager::log(Warning, fmt::format("BusinessSystem: заказ {} снят с точки {}, владелец офлайн — "
                                                 "возврат {} не выдан",
                                                 orderId, businessId, refund));
        }
    }
}

// ------------------------------------------------------------------ лот аукциона

std::string BusinessSystem::labelText(const BusinessService::Business &business, const std::string &ownerName) const
{
    std::string status;
    if (business.owner.empty())
    {
        // Цена 0 — точка государством не выставлена. «Продаётся» на ней звало бы
        // игрока к пикапу, который откажет.
        status = business.price > 0 ? "Продаётся" : "Не продаётся";
    }
    else
    {
        // Ник — КЛИЕНТСКИЙ текст, а лейбл рендерит клиент: цветокоды {RRGGBB} и '~'
        // он интерпретирует, поэтому чистим их как в чате (см. Encoding).
        const std::string safeName = Encoding::neutralizeColorCodes(std::string_view(ownerName));
        status = safeName.empty() ? std::string("Частная собственность") : fmt::format("Владелец: {}", safeName);
    }
    return fmt::format("{}\nБизнес #{}\n{}", m_businessService.typeName(business.type), business.id, status);
}

std::string BusinessSystem::onlineNameOf(const std::string &ownerKey) const
{
    if (ownerKey.empty())
    {
        return {};
    }
    const auto accountId = static_cast<PlayerSessionService::AccountId>(std::strtoll(ownerKey.c_str(), nullptr, 10));
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return {};
    }
    const int playerId = m_sessionService.playerByAccount(accountId);
    if (playerId < 0)
    {
        return {};
    }
    const IPlayer *player = m_core.getPlayers().get(playerId);
    return player ? player->getName().to_string() : std::string();
}

void BusinessSystem::refreshBusinessLabel(int businessId, const std::string &ownerName)
{
    const auto it = m_runtime.find(businessId);
    if (it == m_runtime.end() || it->second.label < 0)
    {
        return; // рантайма нет (точка не заведена) — нечего обновлять
    }
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        return;
    }
    m_labelService.setText(it->second.label, u(labelText(*business, ownerName)));
}

void BusinessSystem::showPurchaseOffer(IPlayer &player, int businessId)
{
    const int playerId = player.getID();
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business || !business->owner.empty())
    {
        return; // бизнес исчез/занят между событиями — молча
    }
    // Цена 0 — бизнес НЕ выставлен государством. Отдавать его даром нельзя.
    if (business->price <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот бизнес не продаётся: госцена не назначена"));
        return;
    }

    std::string body = fmt::format("Бизнес #{}\nТип: {}\n\n", businessId, m_businessService.typeName(business->type));
    body += fmt::format("Государственная цена: {}\n\n", Money::text(business->price));
    body += "Деньги спишутся сразу, бизнес сразу станет вашим.\nВ одни руки — один бизнес.";

    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Покупка бизнеса", body, "Купить", "Закрыть"),
                         [this, playerId, businessId](DialogResponse response, int, StringView)
                         {
                             IPlayer *buyer = m_core.getPlayers().get(playerId);
                             if (!buyer || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             buyBusiness(*buyer, businessId);
                         });
}

void BusinessSystem::buyBusiness(IPlayer &player, int businessId)
{
    const std::string ownerKey = ownerKeyOf(player.getID());
    if (ownerKey.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Покупка доступна только под аккаунтом"));
        return;
    }

    // ГЕЙТ ЗАГРУЗКИ ВЛАДЕНИЯ. Пока зеркало business_owner не пришло из БД,
    // ownsBusiness врёт «бизнеса нет» ВСЕМ, а персист владения делает
    // DELETE ... WHERE business_id = X OR account_id = A — покупка вторым бизнесом
    // стёрла бы строку настоящего бизнеса покупателя. Раньше этот путь гейтился
    // через ready() аукциона; прямой покупке гейт нужен свой.
    if (!m_businessService.isOwnershipLoaded())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Покупка бизнесов временно недоступна — попробуйте через минуту"));
        return;
    }

    // Ре-валидация на клике: пока висел диалог, бизнес могли купить, снести, а цену
    // сменить дев-меню.
    const BusinessService::Business *business = m_businessService.getBusiness(businessId);
    if (!business)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этого бизнеса больше нет"));
        return;
    }
    if (!business->owner.empty())
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот бизнес уже купили"));
        return;
    }
    if (business->price <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Этот бизнес не продаётся: госцена не назначена"));
        return;
    }
    if (m_businessService.ownsBusiness(ownerKey))
    {
        player.sendClientMessage(ERROR_COLOUR, u("У вас уже есть бизнес — второй в одни руки не даётся"));
        return;
    }

    const std::int64_t price = business->price;
    // Деньги забираем ДО выдачи владения: обратный порядок отдал бы бизнес без оплаты.
    if (!m_moneyService.take(player, static_cast<unsigned long long>(price)))
    {
        player.sendClientMessage(ERROR_COLOUR, u(fmt::format("Не хватает денег: нужно {}", Money::text(price))));
        return;
    }

    // Уцелевшие с прежней схемы ставки — уже списанные деньги живых игроков. Бизнес
    // уходит покупателю, торги по нему кончились: сервис вернёт суммы. На чистой базе
    // это no-op.
    const int category = m_auctionService.categoryByKey(AUCTION_KEY);
    if (category >= 0)
    {
        m_auctionService.closeLot(static_cast<std::size_t>(category), businessId,
                                  fmt::format("Бизнес #{} продан по госцене", businessId));
    }

    // setOwner дёрнет onOwnerChanged — тот запишет владение в БД.
    if (!m_businessService.setOwner(businessId, ownerKey))
    {
        m_moneyService.giveMoney(player, static_cast<unsigned long long>(price));
        player.sendClientMessage(ERROR_COLOUR, u("Бизнес исчез — деньги возвращены"));
        return;
    }
    player.sendClientMessage(
        INFO_COLOUR, u(fmt::format("Бизнес #{} куплен за {}. Теперь он ваш", businessId, Money::text(price))));
}

bool BusinessSystem::describeLot(int /*businessId*/, AuctionService::Lot & /*out*/) const
{
    // Госимущество на торги НЕ выставляется — бизнес покупается по госцене на месте.
    // Категория остаётся зарегистрированной: см. HouseSystem::describeLot.
    return false;
}

// ------------------------------------------------------------------ персист владения (БД)

void BusinessSystem::onOwnerChanged(int businessId, const std::string &oldKey, const std::string &newKey)
{
    // Лейбл перерисовываем оптимистично сразу, до ответа БД — как иконку у домов.
    // Имя нового владельца берём из его живой сессии: покупка возможна только
    // онлайн-игроку, так что ник здесь известен всегда.
    refreshBusinessLabel(businessId, onlineNameOf(newKey));

    if (newKey.empty())
    {
        eraseOwnershipRow(businessId); // выселение — просто снять строку
        return;
    }

    PlayerSessionService::AccountId accountId = PlayerSessionService::NO_ACCOUNT;
    try
    {
        accountId = std::stoll(newKey);
    }
    catch (...)
    {
        return; // ключ не число — не наш формат, персист невозможен
    }

    DatabaseManager::throwQueryOrdered(
        ownershipKey(businessId),
        [businessId, accountId, claimedAt = TimeFormat::nowUnix()](mysqlx::Schema schema)
        {
            mysqlx::Table table = schema.getTable("business_owner");
            mysqlx::Session &dbSession = schema.getSession();
            dbSession.startTransaction();
            try
            {
                // DELETE по бизнесу И по аккаунту + INSERT в одной транзакции:
                // либо владение применилось целиком, либо не осталось следа.
                table.remove()
                    .where("business_id = :business OR account_id = :account")
                    .bind("business", businessId)
                    .bind("account", accountId)
                    .execute();
                table.insert("business_id", "account_id", "claimed_at")
                    .values(businessId, accountId, claimedAt)
                    .execute();
                dbSession.commit();
            }
            catch (...)
            {
                dbSession.rollback(); // не оставляем частичную запись владения
                throw;                // errorCallback откатит память
            }
        },
        [this, businessId, newKey, oldKey](const std::string &error)
        {
            LogManager::log(Error, fmt::format("BusinessSystem: не удалось записать владельца бизнеса {}: {}",
                                               businessId, error));
            // Откат БЕЗ нотификации (setOwnerSilent): иначе рекурсия запустила бы
            // ещё одну попытку и при затяжном сбое БД зациклила бы откаты.
            const BusinessService::Business *business = m_businessService.getBusiness(businessId);
            if (business && business->owner == newKey) // владелец мог смениться за время запроса
            {
                m_businessService.setOwnerSilent(businessId, oldKey);
                // Прежний владелец мог быть офлайн — тогда ника нет и лейбл покажет
                // «Частная собственность» до перезапуска. Это путь сбоя БД, не штатный.
                refreshBusinessLabel(businessId, onlineNameOf(oldKey));
            }
        });
}

void BusinessSystem::eraseOwnershipRow(int businessId)
{
    DatabaseManager::throwQueryOrdered(
        ownershipKey(businessId),
        [businessId](mysqlx::Schema schema)
        {
            schema.getTable("business_owner")
                .remove()
                .where("business_id = :business")
                .bind("business", businessId)
                .execute();
        },
        [businessId](const std::string &error)
        {
            // Память уже без владельца; осиротевшая строка перезапишется при
            // следующей раздаче этого id (DELETE идёт перед INSERT).
            LogManager::log(Error, fmt::format("BusinessSystem: не удалось снять владение бизнеса {}: {}", businessId,
                                               error));
        });
}

void BusinessSystem::loadOwnershipAsync()
{
    // business_id, account_id, ник владельца ("" — игрока в player нет либо name NULL).
    using OwnerRow = std::tuple<int, std::int64_t, std::string>;
    DatabaseManager::selectQuery<std::vector<OwnerRow>>(
        [](mysqlx::Schema schema)
        {
            // Имя владельца — для 3D-текста у входа. Именно LEFT JOIN: при INNER
            // строка владения без записи в player (аккаунт удалён вручную) выпала бы
            // из выборки, точка в памяти стала бы ничейной и её продали бы второй раз.
            // Отдельной колонки под имя не заводим — она разъезжалась бы со сменой
            // ника, а player.name и так источник правды.
            mysqlx::SqlResult rows = schema.getSession()
                                         .sql("SELECT o.business_id, o.account_id, p.name "
                                              "FROM business_owner o LEFT JOIN player p ON p.id = o.account_id")
                                         .execute();
            std::vector<OwnerRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                // Каждая строка под своим try: порченое поле теряет ТОЛЬКО её, а
                // не всё владение разом.
                try
                {
                    std::string ownerName;
                    if (!row.get(2).isNull())
                    {
                        ownerName = row.get(2).get<std::string>();
                    }
                    result.emplace_back(row.get(0).get<int>(), row.get(1).get<std::int64_t>(), std::move(ownerName));
                }
                catch (...)
                {
                    continue;
                }
            }
            return result;
        },
        [this](std::vector<OwnerRow> owners)
        {
            for (const auto &[businessId, accountId, ownerName] : owners)
            {
                if (accountId == PlayerSessionService::NO_ACCOUNT || !m_businessService.getBusiness(businessId))
                {
                    continue; // не аккаунт либо осиротевшая строка (бизнес снесён)
                }
                // Загрузка зеркала — БЕЗ нотификации: иначе каждая строка
                // спровоцировала бы write-through обратно в ту же БД.
                m_businessService.setOwnerSilent(businessId, std::to_string(accountId));
                refreshBusinessLabel(businessId, ownerName); // «Продаётся» -> «Владелец: ник»
            }
            m_businessService.markOwnershipLoaded(); // итоги аукционов разблокированы
        },
        [this](const std::string &error)
        {
            LogManager::log(Error, "BusinessSystem: не удалось загрузить владение бизнесами: " + error);
            // Флаг НЕ выставляем: с пустым зеркалом итоги аукциона раздали бы
            // бизнесы игрокам, у которых они уже есть, — необратимо. Торги просто
            // подождут до перезапуска (ставки и деньги при этом целы).
        });
}

// ------------------------------------------------------------------ персист (файл)

void BusinessSystem::loadFromFileAsync()
{
    ThreadPool::Task<std::string> task;
    task.func = []() -> std::string
    {
        std::error_code ec;
        if (!std::filesystem::exists(BUSINESSES_FILE, ec))
        {
            return std::string(); // файла нет — норма для первого запуска
        }
        std::ifstream in(BUSINESSES_FILE, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error("не удалось открыть " + BUSINESSES_FILE);
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    task.callback = [this](std::string content)
    {
        // Владение из БД чейнится ПОСЛЕ разбора описаний на ВСЕХ ветках (пусто/
        // битый/успех): иначе на пустом файле зеркало не подтянулось бы вовсе и
        // итоги аукционов остались бы заблокированы навсегда.
        if (content.empty())
        {
            loadOwnershipAsync();
            return;
        }
        bool ok = false;
        const std::vector<BusinessService::Business> parsed = parse(content, ok);
        if (!ok)
        {
            // Битый файл уводим в .bak и продолжаем с пустым списком: перезапись
            // мусора поверх лишила бы дева шанса его починить руками.
            std::error_code ec;
            std::filesystem::rename(BUSINESSES_FILE, BUSINESSES_BACKUP, ec);
            LogManager::log(Error, "BusinessSystem: битый " + BUSINESSES_FILE + ", уведён в .bak");
            loadOwnershipAsync();
            return;
        }
        for (const BusinessService::Business &business : parsed)
        {
            m_businessService.loadBusiness(business);
        }
        m_businessService.finalizeLoad();
        // Точки заводим ПОСЛЕ загрузки всех записей: spawn читает каталог типа,
        // а типы зарегистрированы ещё в конструкторах систем.
        for (const auto &[id, business] : m_businessService.businesses())
        {
            spawnBusiness(business);
        }
        loadOwnershipAsync();
    };
    task.errorCallback = [this](const std::string &error)
    {
        LogManager::log(Error, "BusinessSystem: ошибка чтения " + BUSINESSES_FILE + ": " + error);
        loadOwnershipAsync();
    };
    ThreadPool::addTask(std::move(task));
}

void BusinessSystem::saveToFileAsync()
{
    ThreadPool::Task<bool> task;
    task.func = [content = m_businessService.serialize()]()
    {
        std::ofstream out(BUSINESSES_FILE, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            throw std::runtime_error("не удалось открыть " + BUSINESSES_FILE + " на запись");
        }
        out << content;
        if (!out.good())
        {
            throw std::runtime_error("ошибка записи " + BUSINESSES_FILE);
        }
        return true;
    };
    task.callback = [](bool) {};
    task.errorCallback = [](const std::string &error)
    {
        LogManager::log(Error, "BusinessSystem: ошибка сохранения " + BUSINESSES_FILE + ": " + error);
    };
    ThreadPool::addTask(std::move(task));
}
