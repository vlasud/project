#include "Systems/HouseSystem/HouseSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
const std::string HOUSES_FILE = "houses.json"; // рабочая директория сервера (рядом с сервером)
const std::string HOUSES_BACKUP = "houses.json.bak";

const Colour DEBUG_COLOUR{170, 255, 170}; // дев-зелёный (как в прочей дев-тулзе)
// Игроковые сообщения занятия дома — игровые цвета (НЕ дев-зелёный): успех —
// INFO_COLOUR, отказы — ERROR_COLOUR (как в FamilySystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};
// Домовые радар-иконки SA: 31 — зелёная (ничейный дом), 32 — красная (занятый).
// Это именные спрайты с фиксированным цветом — переданный Colour они игнорируют,
// цвет задаёт сам тип иконки.
constexpr int HOUSE_ICON_FREE = 31;
constexpr int HOUSE_ICON_OWNED = 32;

// Модели пикапов: видимый домик-маркер у входа, дверь-маркер у выхода. Тип 1 —
// «подбор по касанию, всегда виден» (как у прочих маркеров в проекте).
constexpr int ENTRANCE_PICKUP_MODEL = 19522; // зелёный домик-маркер входа
constexpr int EXIT_PICKUP_MODEL = 1318;      // маркер выхода внутри интерьера
constexpr PickupType PICKUP_TYPE = 1;        // подбор по касанию, всегда виден

// 3D-текст у входа — игроцкий ориентир дома (виден всем у двери). Зелёный 90EE90
// в пару к зелёной иконке (свободно); не белый (выгорает на песке) и не кислотный
// 00FF00. Близкая дистанция отрисовки, чтобы лейблы не засоряли горизонт при
// плотной застройке; 10 м + testLOS=true — текст читается только вблизи и НЕ
// проступает сквозь стены/объекты (та же дистанция, что у бизнесов).
const Colour HOUSE_LABEL_COLOUR{90, 238, 144}; // 90EE90
constexpr float HOUSE_LABEL_DRAW_DISTANCE = 10.0f;
constexpr bool HOUSE_LABEL_TEST_LOS = true;

// Иконка дома на карте стримится только вблизи входа (не засоряет радар издалека):
// появляется в этом радиусе от двери и пропадает за ним.
constexpr float HOUSE_ICON_STREAM_DISTANCE = 150.0f;

// Грейс ре-триггера (общая длительность для входа и выхода): после телепорта в
// интерьер игрок какое-то время не может сработать пикап выхода, а после выхода —
// пикап входа (точка выхода в EXIT_DISTANCE от него). Страховка от ре-триггера на
// лаге позиции.
constexpr std::chrono::milliseconds EXIT_GRACE{1500};
// Смещение пикапа выхода от точки спавна внутри — чтобы появившийся игрок не стоял
// прямо на нём (основной гард, грейс — вторичный).
constexpr float EXIT_PICKUP_OFFSET = 2.0f;

// Стабильный ключ категории в файле аукционов. Строка, а не индекс регистрации:
// порядок систем правится, а ставки обязаны оставаться на своих лотах.
const std::string AUCTION_KEY = "house";

// Потолок стартовой цены: защита от опечатки дева.
constexpr std::int64_t MAX_PRICE = 100000000;

bool finite3(const Vector3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Границы легального мира — как inWorldBounds() в PlayerLocationService. Записи с
// экстремальными/битыми координатами из правленого файла отбрасываются на загрузке.
constexpr float WORLD_BOUND_XY = 20000.0f;
constexpr float WORLD_MIN_Z = -1000.0f;
constexpr float WORLD_MAX_Z = 5000.0f;

bool inWorldBounds(const Vector3 &p)
{
    return p.x >= -WORLD_BOUND_XY && p.x <= WORLD_BOUND_XY && p.y >= -WORLD_BOUND_XY && p.y <= WORLD_BOUND_XY &&
           p.z >= WORLD_MIN_Z && p.z <= WORLD_MAX_Z;
}

// Прочитать Vector3 из json-массива [x,y,z] с проверкой. false — поле не массив
// из трёх чисел, координаты не конечны либо вне границ мира.
bool readVec3(const nlohmann::json &node, Vector3 &out)
{
    if (!node.is_array() || node.size() != 3)
    {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (!node[i].is_number())
        {
            return false;
        }
    }
    out = {node[0].get<float>(), node[1].get<float>(), node[2].get<float>()};
    return finite3(out) && inWorldBounds(out);
}
} // namespace

HouseSystem::HouseSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_mapIconService(serviceRegister.getService<MapIconService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_labelService(serviceRegister.getService<TextLabelService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_auctionService(serviceRegister.getService<AuctionService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("house", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMain(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "дома — дев-меню (создание/список/удаление)",
                 PlayerCommandService::HelpCategory::Hidden);

    // Единая точка персиста владения: итог аукциона и передача/выселение
    // (HomeMenuSystem) идут через один и тот же наблюдатель.
    HouseService &houses = m_serviceRegister.getService<HouseService>();
    houses.subscribeOwnerChanged([this](int houseId, const std::string &oldKey, const std::string &newKey)
                                 { onOwnerChanged(houseId, oldKey, newKey); });

    // Категория «Дома» в общих торгах: саморегистрация. Сами торги (ставки, сроки,
    // окна, деньги, /auc) ведёт AuctionService — отсюда только «как выглядит лот»,
    // «кому можно отдать» и «как передать».
    AuctionService::CategoryDef category;
    category.key = AUCTION_KEY;
    category.name = "Дома";
    category.info = [this](int houseId, AuctionService::Lot &out) { return describeLot(houseId, out); };
    // Сервис берём через регистр внутри колбэков (как везде в этой системе): ссылка
    // из конструктора живёт только до его конца, а колбэки переживают его.
    category.eligible = [this](const std::string &ownerKey)
    {
        return !m_serviceRegister.getService<HouseService>().ownsHouse(ownerKey); // один дом в одни руки
    };
    category.award = [this](int houseId, const std::string &ownerKey)
    {
        // setOwner дёрнет onOwnerChanged — тот и запишет владение в БД.
        m_serviceRegister.getService<HouseService>().setOwner(houseId, ownerKey);
    };
    category.ready = [this]()
    {
        // Пока зеркало владения не пришло из БД, «один дом на аккаунт» проверять
        // нечем, а раздача дома необратима.
        return m_serviceRegister.getService<HouseService>().isOwnershipLoaded();
    };
    m_auctionService.registerCategory(std::move(category));
}

void HouseSystem::initialize(IComponentList * /*components*/)
{
    loadFromFileAsync();
}

// ------------------------------------------------------------------ рантайм-хэндлы

void HouseSystem::spawnHouse(const HouseService::House &house)
{
    // Уже заведён — не дублируем (защита от двойного spawn на одном id).
    if (m_runtime.count(house.id))
    {
        return;
    }

    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const std::vector<HouseService::CatalogEntry> &cat = service.catalog();
    if (!HouseService::catalogValid(house.interiorIndex))
    {
        return; // битый индекс — дом не заводим (страховка, load уже фильтрует)
    }
    const HouseService::CatalogEntry &entry = cat[house.interiorIndex];

    Runtime runtime;

    // Пикап входа — в основном мире (vw 0), на позиции создателя.
    runtime.entrancePickup =
        m_pickupService.add(ENTRANCE_PICKUP_MODEL, PICKUP_TYPE, house.entrance,
                            [this, houseId = house.id](IPlayer &player) { onEntrancePickup(houseId, player); }, 0);

    // Иконка на карте у входа: ничейный — зелёная (31), занятый — красная (32).
    const int iconType = house.owner.empty() ? HOUSE_ICON_FREE : HOUSE_ICON_OWNED;
    runtime.mapIcon =
        m_mapIconService.addGlobal(iconType, house.entrance, Colour::White(), MapIconStyle_Global, HOUSE_ICON_STREAM_DISTANCE);

    // Пикап выхода — в уникальном мире дома, со смещением от точки спавна внутри.
    Vector3 exitPickupPos = entry.insideSpawn;
    exitPickupPos.x += EXIT_PICKUP_OFFSET;
    runtime.exitPickup =
        m_pickupService.add(EXIT_PICKUP_MODEL, PICKUP_TYPE, exitPickupPos,
                            [this, houseId = house.id](IPlayer &player) { onExitPickup(houseId, player); },
                            static_cast<std::uint32_t>(house.virtualWorld));

    // 3D-текст у входа (vw 0): тип интерьера над номером дома. entry.name — utf-8,
    // слово «Дом» в шаблоне тоже utf-8 -> весь текст через u() в cp1251. Сервис
    // санитизирует текст; клиентского ввода у лейбла нет.
    runtime.label = m_labelService.add(u(fmt::format("{}\nДом #{}", entry.name, house.id)), house.entrance,
                                       HOUSE_LABEL_COLOUR, HOUSE_LABEL_DRAW_DISTANCE, HOUSE_LABEL_TEST_LOS);

    m_runtime.emplace(house.id, runtime);
}

void HouseSystem::despawnHouse(int houseId)
{
    const auto it = m_runtime.find(houseId);
    if (it == m_runtime.end())
    {
        return;
    }

    // Перед снятием пикапа выхода вытолкнуть онлайн-игроков, запертых в интерьере
    // (vw дома): иначе после удаления выйти нечем. Дом ещё в сервисе (despawn зовут
    // до removeHouse), берём его vw/точку выхода.
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    if (const HouseService::House *house = service.getHouse(houseId))
    {
        for (IPlayer *p : m_core.getPlayers().entries())
        {
            if (p && m_locationService.getVirtualWorld(p->getID()) == house->virtualWorld)
            {
                m_locationService.teleport(*p, house->exit, 0, 0);
            }
        }
    }

    const Runtime &runtime = it->second;
    if (runtime.entrancePickup >= 0)
    {
        m_pickupService.remove(runtime.entrancePickup);
    }
    if (runtime.exitPickup >= 0)
    {
        m_pickupService.remove(runtime.exitPickup);
    }
    if (runtime.mapIcon >= 0)
    {
        m_mapIconService.removeGlobal(runtime.mapIcon);
    }
    if (runtime.label >= 0)
    {
        m_labelService.remove(runtime.label);
    }
    m_runtime.erase(it);
}

void HouseSystem::refreshHouseIcon(int houseId)
{
    const auto it = m_runtime.find(houseId);
    if (it == m_runtime.end())
    {
        return; // рантайма нет (дом не заведён) — нечего перекрашивать
    }
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house)
    {
        return;
    }

    // Глобальная иконка не имеет update — снимаем старую и заводим новую по
    // актуальному owner. Хэндл в Runtime обновляем (нет утечки/двойного remove).
    Runtime &runtime = it->second;
    if (runtime.mapIcon >= 0)
    {
        m_mapIconService.removeGlobal(runtime.mapIcon);
        runtime.mapIcon = -1;
    }
    const int iconType = house->owner.empty() ? HOUSE_ICON_FREE : HOUSE_ICON_OWNED;
    runtime.mapIcon = m_mapIconService.addGlobal(iconType, house->entrance, Colour::White(), MapIconStyle_Global,
                                                 HOUSE_ICON_STREAM_DISTANCE);
}

// ------------------------------------------------------------------ пикапы

void HouseSystem::onEntrancePickup(int houseId, IPlayer &player)
{
    if (inEntranceGrace(player.getID()))
    {
        return; // только что вышли (точка выхода рядом с входом) — не втягиваем сразу
    }

    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house || !HouseService::catalogValid(house->interiorIndex))
    {
        return; // дом исчез между событиями — игнор
    }

    // Ничейный дом -> торги (не входим): дом не занимается бесплатно, он
    // разыгрывается на аукционе. Окно рисует AuctionSystem — оно общее для всех
    // категорий лотов. Занятый -> молчаливый вход (контроль доступа/замки —
    // будущее: любой занятый дом пускает внутрь).
    if (house->owner.empty())
    {
        const int category = m_auctionService.categoryByKey(AUCTION_KEY);
        if (category >= 0)
        {
            m_auctionService.openLot(player, static_cast<std::size_t>(category), houseId);
        }
        return;
    }

    const HouseService::CatalogEntry &entry = service.catalog()[house->interiorIndex];
    // Вход в дом: телепорт в интерьер (мир дома), включаем грейс выхода.
    m_locationService.teleport(player, entry.insideSpawn, static_cast<unsigned>(entry.interiorId), house->virtualWorld);
    m_exitGraceFrom[player.getID()] = std::chrono::steady_clock::now();
}

bool HouseSystem::describeLot(int houseId, AuctionService::Lot &out) const
{
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house || !house->owner.empty())
    {
        return false; // снесён либо уже чей-то — торгов по нему нет
    }
    out.title = fmt::format("Дом #{}", houseId);
    out.description = fmt::format("Интерьер: {}. Парковка: {} машин", service.catalog()[house->interiorIndex].name,
                                  house->parkingCap);
    out.position = house->entrance;
    out.minPrice = house->price;
    return true;
}

void HouseSystem::onOwnerChanged(int houseId, const std::string &oldKey, const std::string &newKey)
{
    // Иконка перекрашивается оптимистично сразу (как было при занятии) — БД лишь
    // подтверждает write-through, откат при сбое перекрасит обратно.
    refreshHouseIcon(houseId);

    // Владение — в БД (write-through, как членство фракций). newKey непустой:
    // DELETE по дому И по аккаунту + INSERT В ОДНОЙ ТРАНЗАКЦИИ (либо вся пара
    // применилась, либо ничего) — та же атомарность, что раньше была только у
    // занятия, теперь общая для занятия/передачи. newKey пустой (выселение) —
    // просто DELETE строки дома. Список домов (houses.json) НЕ меняется — файл не
    // пишем ни на одном из путей смены владения.
    if (!newKey.empty())
    {
        PlayerSessionService::AccountId accountId = PlayerSessionService::NO_ACCOUNT;
        try
        {
            accountId = std::stoll(newKey);
        }
        catch (...)
        {
            return; // ключ не число — не наш формат ownerKey, персист невозможен
        }
        DatabaseManager::throwQuery(
            [houseId, accountId, claimedAt = static_cast<std::int64_t>(std::time(nullptr))](mysqlx::Schema schema)
            {
                mysqlx::Table table = schema.getTable("house_owner");
                mysqlx::Session &dbSession = schema.getSession();
                dbSession.startTransaction();
                try
                {
                    table.remove()
                        .where("house_id = :house OR account_id = :account")
                        .bind("house", houseId)
                        .bind("account", accountId)
                        .execute();
                    table.insert("house_id", "account_id", "claimed_at")
                        .values(houseId, accountId, claimedAt)
                        .execute();
                    dbSession.commit();
                }
                catch (...)
                {
                    dbSession.rollback(); // не оставляем частичную запись владения
                    throw;                // errorCallback откатит память
                }
            },
            // Запись владения в БД не прошла — откатываем оптимистичную память на
            // oldKey БЕЗ повторной нотификации (setOwnerSilent — иначе рекурсия
            // запустила бы ещё одну БД-попытку и при затяжном сбое БД зациклила бы
            // откаты), перекрашиваем иконку обратно.
            [this, houseId, newKey, oldKey](const std::string &error)
            {
                LogManager::log(Error,
                                fmt::format("HouseSystem: failed to persist owner change of house {}: {}", houseId,
                                            error));
                HouseService &service = m_serviceRegister.getService<HouseService>();
                const HouseService::House *house = service.getHouse(houseId);
                if (house && house->owner == newKey) // дом мог смениться ещё раз за время запроса
                {
                    service.setOwnerSilent(houseId, oldKey);
                    refreshHouseIcon(houseId);
                }
            });
    }
    else
    {
        DatabaseManager::throwQuery(
            [houseId](mysqlx::Schema schema)
            { schema.getTable("house_owner").remove().where("house_id = :house").bind("house", houseId).execute(); },
            [houseId](const std::string &error)
            {
                LogManager::log(Error, fmt::format("HouseSystem: failed to delete ownership of house {}: {}",
                                                    houseId, error));
                // Выселение уже применено в памяти/иконке — оставляем как есть.
                // Осиротевшая строка house_owner (дом снова занят при следующей
                // попытке) снимется её же DELETE-веткой транзакции занятия.
            });
    }
}

void HouseSystem::onExitPickup(int houseId, IPlayer &player)
{
    if (inExitGrace(player.getID()))
    {
        return; // только что вошли — не выкидываем сразу
    }

    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house)
    {
        return; // дом исчез — игнор
    }

    // Выход: телепорт на точку выхода в основном мире (интерьер 0, vw 0), включаем
    // грейс входа (точка выхода в EXIT_DISTANCE от пикапа входа).
    m_locationService.teleport(player, house->exit, 0, 0);
    m_entranceGraceFrom[player.getID()] = std::chrono::steady_clock::now();
}

bool HouseSystem::inExitGrace(int playerId) const
{
    return std::chrono::steady_clock::now() - m_exitGraceFrom[playerId] < EXIT_GRACE;
}

bool HouseSystem::inEntranceGrace(int playerId) const
{
    return std::chrono::steady_clock::now() - m_entranceGraceFrom[playerId] < EXIT_GRACE;
}

// ------------------------------------------------------------------ дев-меню

void HouseSystem::showMain(IPlayer &player)
{
    // Динамические индексы пунктов (не магические числа): порядок строк = порядок
    // веток в switch ниже.
    enum MainItem
    {
        ItemCreate = 0,
        ItemList,
        ItemPickById,
    };

    std::string body;
    body += "Создать дом\n";
    body += "Список домов\n";
    body += "Выбрать дом по id";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Дома — дев-меню", body, "Выбрать", "Закрыть"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return;
            }
            switch (listItem)
            {
            case ItemCreate:
                showCreatePicker(*player);
                break;
            case ItemList:
                showHouseList(*player);
                break;
            case ItemPickById:
                showPickById(*player);
                break;
            default:
                break;
            }
        });
}

void HouseSystem::showPickById(IPlayer &player)
{
    // INPUT id -> то же подменю, что и из списка. Сервис сам парсит целое (мусор/
    // overflow -> повтор показа), id из ввода проверяем через getHouse (несуществующий
    // -> сообщение, не падение). Это холодный дев-путь (по диалогу), не per-tick.
    m_dialogService.showNumberInput(
        player, makeDialog(DialogStyle_INPUT, "Выбрать дом по id", "Введите id дома.", "Открыть", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMain(*player); // «Назад» — в главное меню
                return;
            }

            const HouseService &service = m_serviceRegister.getService<HouseService>();
            // value уже целое; кламп к int не нужен — getHouse сам отсеет любой id
            // вне множества домов (включая значения за пределами int).
            if (value < 1 || value > HouseService::MAX_HOUSE_ID || !service.getHouse(static_cast<int>(value)))
            {
                player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", value)));
                showMain(*player);
                return;
            }
            showHouseMenu(*player, static_cast<int>(value));
        });
}

void HouseSystem::showCreatePicker(IPlayer &player)
{
    std::string body;
    const std::vector<HouseService::CatalogEntry> &cat = HouseService::catalog();
    for (const HouseService::CatalogEntry &entry : cat)
    {
        body += std::string(entry.name) + "\n";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Создать дом — интерьер", body, "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMain(*player);
                return;
            }
            // listItem = interiorIndex; его валидность проверит createHouseFor/createHouse.
            showCapInput(*player, listItem);
        });
}

void HouseSystem::showCapInput(IPlayer &player, int interiorIndex)
{
    // Ввод лимита парковки после выбора интерьера. Сервис сам парсит целое (мусор/
    // пусто/overflow -> повтор показа); ДИАПАЗОН проверяем здесь и при промахе
    // перепоказываем этот же ввод (не создаём дом). Холодный дев-путь, не per-tick.
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Лимит парковки",
                   fmt::format("Сколько машин можно парковать у этого дома? Учитываются и личные машины "
                               "владельца, и расшаренные им семье.\nДопустимо от {} до {}.",
                               HouseService::MIN_PARKING_CAP, HouseService::MAX_PARKING_CAP),
                   "Создать", "Назад"),
        [this, playerId = player.getID(), interiorIndex](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return; // дев вышел на вводе — цепочка обрывается
            }
            if (response != DialogResponse_Left)
            {
                showCreatePicker(*player); // «Назад» — обратно к выбору интерьера
                return;
            }
            // Сюда доходит уже целое; проверяем диапазон и при промахе ре-показываем.
            if (value < HouseService::MIN_PARKING_CAP || value > HouseService::MAX_PARKING_CAP)
            {
                player->sendClientMessage(
                    DEBUG_COLOUR, u(fmt::format("Лимит должен быть от {} до {}. Попробуйте снова",
                                                HouseService::MIN_PARKING_CAP, HouseService::MAX_PARKING_CAP)));
                showCapInput(*player, interiorIndex);
                return;
            }
            showPriceInput(*player, interiorIndex, static_cast<int>(value));
        });
}

void HouseSystem::showPriceInput(IPlayer &player, int interiorIndex, int parkingCap)
{
    // Дом не продаётся напрямую — он разыгрывается на аукционе (см. Docs/Auction.md).
    // Цена дева здесь — СТАРТОВАЯ ПЛАНКА торгов: ниже неё ставку не примут.
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Стартовая цена",
                   "С какой суммы начинаются торги за этот дом?\nНиже неё ставку не примут. 0 — торги с $1.",
                   "Создать", "Назад"),
        [this, playerId = player.getID(), interiorIndex, parkingCap](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return; // дев вышел на вводе — цепочка обрывается
            }
            if (response != DialogResponse_Left)
            {
                showCapInput(*player, interiorIndex); // «Назад» — обратно к лимиту парковки
                return;
            }
            if (value < 0 || value > MAX_PRICE)
            {
                player->sendClientMessage(DEBUG_COLOUR,
                                          u(fmt::format("Цена — целое от 0 до {}. Попробуйте снова", MAX_PRICE)));
                showPriceInput(*player, interiorIndex, parkingCap);
                return;
            }
            createHouseFor(*player, interiorIndex, parkingCap, value);
        });
}

void HouseSystem::showHouseList(IPlayer &player)
{
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const std::vector<HouseService::CatalogEntry> &cat = service.catalog();

    // Стабильный порядок: по id (unordered_map не упорядочен). list-индекс ->
    // houseId через локальный вектор, который захватываем в колбэк.
    std::vector<int> ids;
    ids.reserve(service.houses().size());
    for (const auto &[id, house] : service.houses())
    {
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    std::string body;
    for (const int id : ids)
    {
        const HouseService::House *house = service.getHouse(id);
        const char *name = (house && HouseService::catalogValid(house->interiorIndex))
                               ? cat[house->interiorIndex].name
                               : "?";
        body += fmt::format("#{} - {}\n", id, name);
    }
    if (ids.empty())
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Домов пока нет"));
        showMain(player);
        return;
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Список домов", body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), ids = std::move(ids)](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left || listItem < 0 || listItem >= static_cast<int>(ids.size()))
            {
                showMain(*player);
                return;
            }
            showHouseMenu(*player, ids[listItem]);
        });
}

void HouseSystem::showHouseMenu(IPlayer &player, int houseId)
{
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house)
    {
        player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", houseId)));
        showMain(player);
        return;
    }
    const char *name = HouseService::catalogValid(house->interiorIndex)
                           ? service.catalog()[house->interiorIndex].name
                           : "?";

    // Кап показываем в теле, чтобы дев видел значение без чтения houses.json.
    std::string body;
    body += fmt::format("Лимит парковки: {}\n", house->parkingCap);
    body += "Телепорт ко входу\n";
    body += "Удалить дом";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Дом #{} — {}", houseId, name), body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), houseId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showHouseList(*player);
                return;
            }

            const HouseService &service = m_serviceRegister.getService<HouseService>();
            const HouseService::House *house = service.getHouse(houseId);
            if (!house)
            {
                player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", houseId)));
                showMain(*player);
                return;
            }

            // Пункт 0 — «Лимит парковки» (информационная строка): без действия.
            switch (listItem)
            {
            case 1: // стартовая цена торгов — правится и у уже созданного дома
                showHousePriceEdit(*player, houseId);
                break;
            case 2: // телепорт ко входу (дев проходит через пикап и проверяет вход)
                m_locationService.teleport(*player, house->entrance, 0, 0);
                player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Телепорт ко входу дома #{}", houseId)));
                break;
            case 3:
                showDeleteConfirm(*player, houseId);
                break;
            default:
                break;
            }
        });
}

void HouseSystem::showHousePriceEdit(IPlayer &player, int houseId)
{
    // Дома из старых houses.json стартуют торги с нуля — без этой правки их
    // пришлось бы пересоздавать, чтобы задать планку.
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, fmt::format("Дом #{} — стартовая цена", houseId),
                   "С какой суммы начинаются торги за этот дом?\nНиже неё ставку не примут. 0 — торги с $1.",
                   "Сохранить", "Назад"),
        [this, playerId = player.getID(), houseId](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showHouseMenu(*player, houseId);
                return;
            }
            if (value < 0 || value > MAX_PRICE)
            {
                player->sendClientMessage(DEBUG_COLOUR,
                                          u(fmt::format("Цена — целое от 0 до {}. Попробуйте снова", MAX_PRICE)));
                showHousePriceEdit(*player, houseId);
                return;
            }
            if (!m_serviceRegister.getService<HouseService>().setPrice(houseId, value))
            {
                player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", houseId)));
                showMain(*player);
                return;
            }
            saveToFileAsync();
            player->sendClientMessage(DEBUG_COLOUR,
                                      u(fmt::format("Дом #{}: старт торгов теперь ${}", houseId, value)));
            showHouseMenu(*player, houseId);
        });
}

void HouseSystem::showDeleteConfirm(IPlayer &player, int houseId)
{
    const HouseService &service = m_serviceRegister.getService<HouseService>();
    const HouseService::House *house = service.getHouse(houseId);
    if (!house)
    {
        player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", houseId)));
        showMain(player);
        return;
    }
    const char *name = HouseService::catalogValid(house->interiorIndex)
                           ? service.catalog()[house->interiorIndex].name
                           : "?";

    const std::string body = fmt::format("Удалить дом #{} ({})?\nЭто действие нельзя отменить.", houseId, name);

    m_dialogService.show(
        player, makeDialog(DialogStyle_MSGBOX, "Удаление дома", body, "Удалить", "Назад"),
        [this, playerId = player.getID(), houseId](DialogResponse response, int, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showHouseMenu(*player, houseId);
                return;
            }
            deleteHouse(*player, houseId);
        });
}

// ------------------------------------------------------------------ операции дев-меню

void HouseSystem::createHouseFor(IPlayer &player, int interiorIndex, int parkingCap, std::int64_t price)
{
    if (!HouseService::catalogValid(interiorIndex))
    {
        player.sendClientMessage(DEBUG_COLOUR, u("Неверный интерьер"));
        showMain(player);
        return;
    }

    HouseService &service = m_serviceRegister.getService<HouseService>();
    const int playerId = player.getID();
    // Позиция/угол — серверные (источник правды), не клиент-доверенные.
    const Vector3 pos = m_locationService.getPosition(playerId);
    const float angle = player.getRotation().ToEuler().z; // yaw, градусы

    // parkingCap клампит сам createHouse — берём фактический (клампнутый) из house->parkingCap.
    const HouseService::House *house = service.createHouse(pos, angle, interiorIndex, parkingCap, price);
    if (!house)
    {
        // Индекс уже проверен выше — nullptr здесь означает достигнутый лимит id.
        player.sendClientMessage(DEBUG_COLOUR, u("Достигнут лимит домов"));
        showMain(player);
        return;
    }

    const char *name = service.catalog()[interiorIndex].name;
    spawnHouse(*house);
    saveToFileAsync();

    player.sendClientMessage(
        DEBUG_COLOUR, u(fmt::format("Дом #{} создан ({}). Лимит парковки — {}, старт торгов — ${}. "
                                    "Вход — на вашей позиции, выход — за спиной",
                                    house->id, name, house->parkingCap, house->price)));
}

void HouseSystem::deleteHouse(IPlayer &player, int houseId)
{
    HouseService &service = m_serviceRegister.getService<HouseService>();
    if (!service.getHouse(houseId))
    {
        player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} не найден", houseId)));
        showMain(player);
        return;
    }

    // Ставки на этот дом — уже списанные деньги игроков: закрываем торги ДО
    // стирания записи, общий сервис вернёт суммы владельцам ставок.
    const int category = m_auctionService.categoryByKey(AUCTION_KEY);
    if (category >= 0)
    {
        m_auctionService.closeLot(static_cast<std::size_t>(category), houseId,
                                  fmt::format("Дом #{} снесён", houseId));
    }
    // Снимаем рантайм-хэндлы ДО стирания из сервиса (нет утечки/двойного remove).
    despawnHouse(houseId);
    service.removeHouse(houseId);
    saveToFileAsync(); // список домов (houses.json)
    // Владение — отдельный источник правды (БД). Стираем строку владения, чтобы
    // не осталось осиротевшего владения (дома уже нет, а house_owner ссылалась бы
    // на него — и блокировала бы аккаунт по UNIQUE).
    DatabaseManager::throwQuery(
        [houseId](mysqlx::Schema schema)
        { schema.getTable("house_owner").remove().where("house_id = :house").bind("house", houseId).execute(); },
        [houseId](const std::string &error) {
            LogManager::log(Error,
                            fmt::format("HouseSystem: failed to delete ownership of house {}: {}", houseId, error));
        });

    player.sendClientMessage(DEBUG_COLOUR, u(fmt::format("Дом #{} удалён", houseId)));
    showMain(player);
}

// ------------------------------------------------------------------ файлы
// Диск — на воркере тредпула; разбор и пересоздание контента — на главном потоке.

void HouseSystem::loadFromFileAsync()
{
    ThreadPool::Task<std::string> task;
    task.func = []() -> std::string
    {
        std::error_code ec;
        if (!std::filesystem::exists(HOUSES_FILE, ec))
        {
            return std::string(); // файла нет — пусто (норма для первого запуска)
        }
        std::ifstream in(HOUSES_FILE, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error("не удалось открыть " + HOUSES_FILE);
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    task.callback = [this](std::string content)
    {
        // Владение из БД грузим ВСЕГДА после разбора описаний — на любой ветке
        // (пусто / битый файл / успех). getHouse-гард в колбэке loadOwnershipAsync
        // отбросит осиротевшие записи, если домов в памяти нет. Иначе на пустом/
        // битом файле владение не подтянулось бы вовсе и флаг загрузки не встал.
        if (content.empty())
        {
            loadOwnershipAsync();
            return; // пустой/отсутствующий файл — домов нет
        }

        bool ok = false;
        std::vector<HouseService::House> parsed = parse(content, ok);
        if (!ok)
        {
            // Битый/мусорный файл: переименовываем в .bak и продолжаем с пустым
            // списком (не падаем, не теряем шанс на разбор вручную).
            LogManager::log(Warning,
                            "HouseSystem: повреждён " + HOUSES_FILE + ", переименован в " + HOUSES_BACKUP);
            std::error_code ec;
            std::filesystem::rename(HOUSES_FILE, HOUSES_BACKUP, ec);
            loadOwnershipAsync();
            return;
        }

        HouseService &service = m_serviceRegister.getService<HouseService>();
        for (const HouseService::House &house : parsed)
        {
            service.loadHouse(house);
        }
        service.finalizeLoad();

        // Пересоздаём рантайм-хэндлы для каждого загруженного дома (все ничейные —
        // owner ещё пуст; иконки зелёные).
        for (const auto &[id, house] : service.houses())
        {
            spawnHouse(house);
        }

        // Владение — отдельный источник правды (БД). Грузим house_owner ТОЛЬКО
        // после спавна домов: их рантайм-иконки уже есть, и применение владения
        // перекрасит зелёную в красную (refreshHouseIcon найдёт m_runtime).
        loadOwnershipAsync();
    };
    task.errorCallback = [](const std::string &error)
    { LogManager::log(Error, "HouseSystem: ошибка чтения houses.json: " + error); };
    ThreadPool::addTask(std::move(task));
}

void HouseSystem::loadOwnershipAsync()
{
    // Все строки house_owner: (house_id, account_id). Один selectQuery на старте —
    // воркер вычитывает в владеющий вектор, колбэк на главном потоке применяет.
    using OwnerRow = std::pair<int, std::int64_t>;
    DatabaseManager::selectQuery<std::vector<OwnerRow>>(
        [](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("house_owner").select("house_id", "account_id").execute();
            std::vector<OwnerRow> result;
            while (mysqlx::Row row = rows.fetchOne())
            {
                // Каждую строку читаем под try/catch: порченое поле (NULL/тип/
                // конвертация) пропускает ТОЛЬКО эту строку, а не валит всю выборку
                // — одна битая запись не теряет всё владение.
                try
                {
                    result.emplace_back(row.get(0).get<int>(), row.get(1).get<std::int64_t>());
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
            // Главный поток: дома уже заспавнены (цепочка после спавна). Для каждой
            // живой записи владения — выставить owner в памяти (БЕЗ записи в БД, это
            // загрузка) и перекрасить иконку (зелёная -> красная). getHouse-гард:
            // строка владения могла осиротеть (дом удалён из houses.json).
            HouseService &service = m_serviceRegister.getService<HouseService>();
            for (const auto &[houseId, accountId] : owners)
            {
                if (accountId == PlayerSessionService::NO_ACCOUNT)
                {
                    continue; // 0 — не аккаунт (ничейный), не применяем
                }
                if (!service.getHouse(houseId))
                {
                    continue; // дома из houses.json больше нет — осиротевшее владение
                }
                // Загрузка зеркала из БД — БЕЗ нотификации onOwnerChanged (иначе
                // каждая строка спровоцировала бы избыточный write-through обратно
                // в ту же БД, откуда она только что прочитана).
                service.setOwnerSilent(houseId, std::to_string(accountId));
                refreshHouseIcon(houseId);
            }
            // Владение в памяти — занятие домов разблокировано; оповещаем
            // подписчиков (SpawnChoiceSystem перерезолвит спавн «Дом» онлайн-игрокам,
            // чей Home-выбор применился на логине ДО прихода house_owner).
            service.markOwnershipLoaded();
        },
        [this](const std::string &error)
        {
            LogManager::log(Error, "HouseSystem: failed to load house ownership: " + error);
            // БД-загрузка упала — НЕ блокируем фичу навсегда: работаем с пустым
            // in-memory владением. БД-бэкстопы (UNIQUE account_id, claim-DELETE по
            // OR account_id) держат консистентность даже при пустом зеркале. Флаг
            // готовности всё равно выставляем (подписчики разблокируются).
            m_serviceRegister.getService<HouseService>().markOwnershipLoaded();
        });
}

void HouseSystem::saveToFileAsync()
{
    const HouseService &service = m_serviceRegister.getService<HouseService>();

    ThreadPool::Task<bool> task;
    task.func = [content = service.serialize()]()
    {
        std::ofstream out(HOUSES_FILE, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            throw std::runtime_error("не удалось открыть " + HOUSES_FILE + " на запись");
        }
        out << content;
        if (!out.good())
        {
            throw std::runtime_error("ошибка записи " + HOUSES_FILE);
        }
        return true;
    };
    // Успех — молча (дев уже получил сообщение о создании/удалении), но callback
    // обязателен: Task<bool>::call() зовёт его при успехе (пустой даст bad_function_call).
    task.callback = [](bool) {};
    task.errorCallback = [](const std::string &error)
    { LogManager::log(Error, "HouseSystem: ошибка сохранения houses.json: " + error); };
    ThreadPool::addTask(std::move(task));
}

std::vector<HouseService::House> HouseSystem::parse(const std::string &content, bool &ok)
{
    // Крах-безопасный разбор через nlohmann/json: на любой мусор/обрезку отдаём то,
    // что распозналось, и НЕ кидаем исключение наружу. ok=false — контент не наш
    // JSON (повод переименовать файл в .bak в колбэке).
    ok = false;
    std::vector<HouseService::House> result;

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(content);
    }
    catch (...)
    {
        return result; // битый JSON — ok остаётся false
    }

    if (!root.is_array())
    {
        return result; // не наш формат
    }

    ok = true; // JSON распознан как массив — дальше пропускаем битые элементы
    for (const nlohmann::json &item : root)
    {
        if (!item.is_object())
        {
            continue;
        }

        HouseService::House house;

        // id обязан попадать в (0, MAX_HOUSE_ID]: за потолком VW_BASE + id переполнил
        // бы int (UB). Источник правды id — сервер; в файле — порядковый.
        if (!item.contains("id") || !item["id"].is_number_integer())
        {
            continue;
        }
        const int id = item["id"].get<int>();
        if (!(id > 0 && id <= HouseService::MAX_HOUSE_ID))
        {
            continue;
        }
        house.id = id;

        // interiorIndex обязан попадать в каталог.
        if (!item.contains("interiorIndex") || !item["interiorIndex"].is_number_integer() ||
            !HouseService::catalogValid(item["interiorIndex"].get<int>()))
        {
            continue;
        }
        house.interiorIndex = item["interiorIndex"].get<int>();

        // entrance/exit — массивы из трёх конечных чисел.
        if (!item.contains("entrance") || !readVec3(item["entrance"], house.entrance))
        {
            continue;
        }
        if (!item.contains("exit") || !readVec3(item["exit"], house.exit))
        {
            continue;
        }

        // Остальные поля — с дефолтами; невалидные молча заменяются.
        house.exitAngle = 0.0f;
        if (item.contains("exitAngle") && item["exitAngle"].is_number())
        {
            const float angle = item["exitAngle"].get<float>();
            house.exitAngle = std::isfinite(angle) ? angle : 0.0f;
        }

        // parkingCap — необязательное поле. Отсутствует (старые houses.json) или
        // мусор (float/строка/не-int) -> DEFAULT_PARKING_CAP. Всегда кламп в
        // [MIN, MAX] (правленый файл не пробьёт границы, кап=0 невозможен).
        house.parkingCap = HouseService::DEFAULT_PARKING_CAP;
        if (item.contains("parkingCap") && item["parkingCap"].is_number_integer())
        {
            house.parkingCap = std::clamp(item["parkingCap"].get<int>(), HouseService::MIN_PARKING_CAP,
                                          HouseService::MAX_PARKING_CAP);
        }

        // price — необязательное поле (старые houses.json): отсутствует/мусор -> 0,
        // отрицательное клампится. Это стартовая планка аукциона.
        house.price = 0;
        if (item.contains("price") && item["price"].is_number_integer())
        {
            house.price = std::max<std::int64_t>(item["price"].get<std::int64_t>(), 0);
        }

        // vw ВСЕГДА вычисляем (VW_BASE + id), как createHouse. Полю virtualWorld из
        // файла НЕ доверяем (читаемость/диагностика) — иначе правленое значение
        // ломает изоляцию или переполняет int.
        house.virtualWorld = HouseService::VW_BASE + house.id;

        // owner НЕ читаем из файла: владение живёт в БД (house_owner) и применяется
        // отдельно (loadOwnershipAsync) ПОСЛЕ спавна домов. Дом из файла всегда
        // грузится ничейным; legacy-поле owner в старом houses.json игнорируется.
        house.owner.clear();

        result.push_back(std::move(house));
    }

    return result;
}
