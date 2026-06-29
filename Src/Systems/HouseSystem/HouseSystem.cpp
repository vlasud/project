#include "Systems/HouseSystem/HouseSystem.h"

#include "Log/LogManager.h"
#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace
{
const std::string HOUSES_FILE = "houses.json"; // рабочая директория сервера (рядом с сервером)
const std::string HOUSES_BACKUP = "houses.json.bak";

const Colour DEBUG_COLOUR{170, 255, 170}; // дев-зелёный (как в прочей дев-тулзе)
// Домовые радар-иконки SA: 31 — зелёная (ничейный дом), 32 — красная (занят,
// заложено под будущее). Это именные спрайты с фиксированным цветом — переданный
// Colour они игнорируют, цвет задаёт сам тип иконки.
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
// плотной застройке; testLOS=false — виден вплотную у двери без мигания за углом.
const Colour HOUSE_LABEL_COLOUR{90, 238, 144}; // 90EE90
constexpr float HOUSE_LABEL_DRAW_DISTANCE = 20.0f;
constexpr bool HOUSE_LABEL_TEST_LOS = false;

// Грейс ре-триггера (общая длительность для входа и выхода): после телепорта в
// интерьер игрок какое-то время не может сработать пикап выхода, а после выхода —
// пикап входа (точка выхода в EXIT_DISTANCE от него). Страховка от ре-триггера на
// лаге позиции.
constexpr std::chrono::milliseconds EXIT_GRACE{1500};
// Смещение пикапа выхода от точки спавна внутри — чтобы появившийся игрок не стоял
// прямо на нём (основной гард, грейс — вторичный).
constexpr float EXIT_PICKUP_OFFSET = 2.0f;

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

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
      m_labelService(serviceRegister.getService<TextLabelService>())
{
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("house", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMain(player); },
                 PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "дома — дев-меню (создание/список/удаление)",
                 PlayerCommandService::HelpCategory::Hidden);
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
    runtime.mapIcon = m_mapIconService.addGlobal(iconType, house.entrance, Colour::White(), MapIconStyle_Global);

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
    const HouseService::CatalogEntry &entry = service.catalog()[house->interiorIndex];

    // Вход в дом: телепорт в интерьер (мир дома), включаем грейс выхода.
    m_locationService.teleport(player, entry.insideSpawn, static_cast<unsigned>(entry.interiorId), house->virtualWorld);
    m_exitGraceFrom[player.getID()] = std::chrono::steady_clock::now();
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
            createHouseFor(*player, listItem);
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

    std::string body;
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

            switch (listItem)
            {
            case 0: // телепорт ко входу (дев проходит через пикап и проверяет вход)
                m_locationService.teleport(*player, house->entrance, 0, 0);
                player->sendClientMessage(DEBUG_COLOUR, u(fmt::format("Телепорт ко входу дома #{}", houseId)));
                break;
            case 1:
                showDeleteConfirm(*player, houseId);
                break;
            default:
                break;
            }
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

void HouseSystem::createHouseFor(IPlayer &player, int interiorIndex)
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

    const HouseService::House *house = service.createHouse(pos, angle, interiorIndex);
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
        DEBUG_COLOUR,
        u(fmt::format("Дом #{} создан ({}). Вход — на вашей позиции, выход — за спиной", house->id, name)));
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

    // Снимаем рантайм-хэндлы ДО стирания из сервиса (нет утечки/двойного remove).
    despawnHouse(houseId);
    service.removeHouse(houseId);
    saveToFileAsync();

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
        if (content.empty())
        {
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
            return;
        }

        HouseService &service = m_serviceRegister.getService<HouseService>();
        for (const HouseService::House &house : parsed)
        {
            service.loadHouse(house);
        }
        service.finalizeLoad();

        // Пересоздаём рантайм-хэндлы для каждого загруженного дома.
        for (const auto &[id, house] : service.houses())
        {
            spawnHouse(house);
        }
    };
    task.errorCallback = [](const std::string &error)
    { LogManager::log(Error, "HouseSystem: ошибка чтения houses.json: " + error); };
    ThreadPool::addTask(std::move(task));
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

        // vw ВСЕГДА вычисляем (VW_BASE + id), как createHouse. Полю virtualWorld из
        // файла НЕ доверяем (читаемость/диагностика) — иначе правленое значение
        // ломает изоляцию или переполняет int.
        house.virtualWorld = HouseService::VW_BASE + house.id;

        house.owner.clear();
        if (item.contains("owner") && item["owner"].is_string())
        {
            house.owner = item["owner"].get<std::string>();
        }

        result.push_back(std::move(house));
    }

    return result;
}
