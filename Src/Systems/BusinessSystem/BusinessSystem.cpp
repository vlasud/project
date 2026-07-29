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
constexpr float EXIT_PICKUP_OFFSET = 1.5f;

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
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_labelService(serviceRegister.getService<TextLabelService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "business", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showDevMenu(player);
        },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "бизнесы — дев-меню (создание/список/снос)",
        PlayerCommandService::HelpCategory::Hidden);

    // Любое изменение описания (создание/снос/копилка) пишет файл — единая точка
    // персиста, чтобы ни один путь изменения не забыл сохранить.
    m_businessService.subscribeChanged(
        [this]()
        {
            saveToFileAsync();
        });

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
    loadFromFileAsync();
}

// ------------------------------------------------------------------ дев-меню

void BusinessSystem::showDevMenu(IPlayer &player)
{
    const int playerId = player.getID();
    const std::string body = fmt::format("Создать бизнес\nСписок ({})\nСнести бизнес", m_businessService.count());

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

    // Замеренная точка выхода интерьера, если она есть. Нет — прежнее смещение от
    // точки спавна: грубо, но лучше пикапа ровно под ногами появившегося игрока.
    Vector3 exitPickupPos = entry.exitPickup;
    if (exitPickupPos.x == 0.0f && exitPickupPos.y == 0.0f && exitPickupPos.z == 0.0f)
    {
        exitPickupPos = entry.insideSpawn;
        exitPickupPos.x += EXIT_PICKUP_OFFSET;
    }
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
    showBusinessMenu(player, businessId);
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
