#include "Systems/Core/GangZoneEditorSystem/GangZoneEditorSystem.h"

#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
const std::string ZONES_DIR = "gangzones";
const Colour DEFAULT_ZONE_COLOUR = Colour(255, 0, 0, 128);
const Colour FLASH_COLOUR = Colour(255, 255, 255, 200);
constexpr auto FLASH_DURATION = std::chrono::seconds(3);

// Биты PlayerKeyData::keys (классические значения SA).
constexpr uint32_t KEY_SPRINT = 8;
constexpr uint32_t KEY_WALK = 1024;

constexpr float SPRINT_MULTIPLIER = 5.0f;
constexpr float WALK_MULTIPLIER = 0.2f;
constexpr float KEY_STEP_MIN = 0.1f;
constexpr float KEY_STEP_MAX = 50.0f;

constexpr float DEFAULT_ZONE_SIZE = 50.0f; // сторона новой зоны
constexpr float MIN_ZONE_SIZE = 5.0f;      // меньше клавишами не ужать
// Применять изменения (с пересборкой обрезки и перепоказом) не чаще, чем раз
// в столько тиков: живой отклик без спама радара hide/show на каждый тик.
constexpr int REBUILD_INTERVAL_TICKS = 5;

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

// Цвет в форме RRGGBB или RRGGBBAA, допускается префикс # или 0x.
bool parseHexColour(const std::string &input, Colour &out)
{
    std::string text = input;
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
    {
        text.erase(0, 2);
    }
    else if (!text.empty() && text[0] == '#')
    {
        text.erase(0, 1);
    }

    if (text.size() != 6 && text.size() != 8)
    {
        return false;
    }
    for (char c : text)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    uint32_t value = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
    if (text.size() == 6)
    {
        value = (value << 8) | 0x80; // без альфы — полупрозрачная по умолчанию
    }
    out = Colour::FromRGBA(value);
    return true;
}

std::string colourToHex(Colour colour)
{
    return fmt::format("{:08X}", colour.RGBA());
}
} // namespace

GangZoneEditorSystem::GangZoneEditorSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_timerService(serviceRegister.getService<TimerService>()),
      m_gangZoneService(serviceRegister.getService<GangZoneService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

    m_commandService.add("gzone", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             if (!m_gangZoneService.isAvailable())
                             {
                                 player.sendClientMessage(Colour::White(), u("Компонент ганг-зон недоступен"));
                                 return;
                             }

                             // Из режима клавиш /gzone фиксирует зону и возвращает в её меню.
                             if (sessionOf(player).keyMode != KeyMode::None)
                             {
                                 stopKeyMode(player);
                                 showZoneEdit(player);
                                 return;
                             }
                             showMain(player);
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "редактор ганг-зон (меню)",
                         PlayerCommandService::HelpCategory::Hidden);

    // Проверка членства по реальной (обрезанной) геометрии — точка всегда
    // принадлежит максимум одной зоне.
    m_commandService.add("gzhere", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             const Vector3 pos = m_locationService.getPosition(player.getID());
                             const int zoneId = m_gangZoneService.zoneAtPoint(pos);
                             if (zoneId >= 0)
                             {
                                 player.sendClientMessage(Colour::White(), u(fmt::format("Вы в зоне #{}", zoneId)));
                             }
                             else
                             {
                                 player.sendClientMessage(Colour::White(), u("Вы вне зон"));
                             }
                         },
                         PermissionSpec::admin(AdminService::DEVELOPER_LEVEL),
                         "показать, в какой ганг-зоне вы стоите", PlayerCommandService::HelpCategory::Hidden);
}

GangZoneEditorSystem::Session &GangZoneEditorSystem::sessionOf(const IPlayer &player)
{
    return m_sessions[player.getID()];
}

IPlayer *GangZoneEditorSystem::onlinePlayer(int playerId)
{
    return m_core.getPlayers().get(playerId);
}

void GangZoneEditorSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    // Таймер подсветки привязан к игроку — TimerService отменит его сам.
    sessionOf(player) = Session{};
}

// ------------------------------------------------------------------ per-tick

bool GangZoneEditorSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    Session &session = sessionOf(player);
    if (session.keyMode == KeyMode::None)
    {
        return true;
    }

    GangZoneService::ZoneInfo info;
    if (!m_gangZoneService.getZone(session.selectedId, info))
    {
        session.keyMode = KeyMode::None;
        return true;
    }

    const PlayerKeyData &keys = player.getKeyData();
    float multiplier = 1.0f;
    if (keys.keys & KEY_SPRINT)
    {
        multiplier = SPRINT_MULTIPLIER;
    }
    else if (keys.keys & KEY_WALK)
    {
        multiplier = WALK_MULTIPLIER;
    }
    const float step = session.keyStep * multiplier;

    // Работаем через центр и размеры — так и сдвиг, и растяжение симметричны.
    GangZonePos &rect = session.editRect;
    float centerX = (rect.min.x + rect.max.x) * 0.5f;
    float centerY = (rect.min.y + rect.max.y) * 0.5f;
    float width = rect.max.x - rect.min.x;
    float height = rect.max.y - rect.min.y;
    bool changed = false;

    if (session.keyMode == KeyMode::Move)
    {
        // Вверх — север (Y+), вправо — восток (X+): как на радаре/карте.
        if (keys.upDown < 0)
        {
            centerY += step;
            changed = true;
        }
        else if (keys.upDown > 0)
        {
            centerY -= step;
            changed = true;
        }
        if (keys.leftRight > 0)
        {
            centerX += step;
            changed = true;
        }
        else if (keys.leftRight < 0)
        {
            centerX -= step;
            changed = true;
        }
    }
    else // Resize
    {
        if (keys.upDown < 0)
        {
            height += step;
            changed = true;
        }
        else if (keys.upDown > 0)
        {
            height -= step;
            changed = true;
        }
        if (keys.leftRight > 0)
        {
            width += step;
            changed = true;
        }
        else if (keys.leftRight < 0)
        {
            width -= step;
            changed = true;
        }
        width = std::max(width, MIN_ZONE_SIZE);
        height = std::max(height, MIN_ZONE_SIZE);
    }

    if (changed)
    {
        rect.min = {centerX - width * 0.5f, centerY - height * 0.5f};
        rect.max = {centerX + width * 0.5f, centerY + height * 0.5f};
        session.editDirty = true;
    }

    // Троттлинг: пересборка обрезки и перепоказ всем — не чаще раза в N тиков.
    if (session.editDirty)
    {
        if (session.rebuildCooldown > 0)
        {
            --session.rebuildCooldown;
        }
        if (session.rebuildCooldown == 0)
        {
            m_gangZoneService.setZoneRect(session.selectedId, session.editRect.min, session.editRect.max);
            session.editDirty = false;
            session.rebuildCooldown = REBUILD_INTERVAL_TICKS;
        }
    }

    return true;
}

// ------------------------------------------------------------------ действия

void GangZoneEditorSystem::createZoneAt(IPlayer &player)
{
    const Vector3 pos = m_locationService.getPosition(player.getID());
    const float half = DEFAULT_ZONE_SIZE * 0.5f;
    const int zoneId = m_gangZoneService.addZone(Vector2(pos.x - half, pos.y - half),
                                                 Vector2(pos.x + half, pos.y + half), DEFAULT_ZONE_COLOUR);
    if (zoneId < 0)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать зону"));
        showMain(player);
        return;
    }

    Session &session = sessionOf(player);
    session.selectedId = zoneId;
    player.sendClientMessage(Colour::White(), u(fmt::format("Зона #{} создана. Откройте карту (ESC) или радар.", zoneId)));

    // Сразу в режим перемещения — обычно зону тут же подгоняют по месту.
    startKeyMode(player, KeyMode::Move);
}

void GangZoneEditorSystem::startKeyMode(IPlayer &player, KeyMode mode)
{
    Session &session = sessionOf(player);
    GangZoneService::ZoneInfo info;
    if (!m_gangZoneService.getZone(session.selectedId, info))
    {
        showMain(player);
        return;
    }

    session.keyMode = mode;
    session.editRect = info.rect;
    session.editDirty = false;
    session.rebuildCooldown = 0; // первое изменение применяется мгновенно

    if (mode == KeyMode::Move)
    {
        player.sendClientMessage(
            Colour::White(), u("Стрелки двигают зону (вверх — север). Sprint — крупно, Alt — точно. /gzone — готово."));
    }
    else
    {
        player.sendClientMessage(
            Colour::White(),
            u("Вверх/вниз — высота зоны, влево/вправо — ширина. Sprint — крупно, Alt — точно. /gzone — готово."));
    }
}

void GangZoneEditorSystem::stopKeyMode(IPlayer &player)
{
    Session &session = sessionOf(player);
    if (session.keyMode == KeyMode::None)
    {
        return;
    }

    if (session.editDirty)
    {
        m_gangZoneService.setZoneRect(session.selectedId, session.editRect.min, session.editRect.max);
        session.editDirty = false;
    }
    session.keyMode = KeyMode::None;
}

void GangZoneEditorSystem::highlightZone(IPlayer &player, int zoneId)
{
    Session &session = sessionOf(player);

    // Прошлая подсветка ещё мигает — гасим её таймер и саму подсветку.
    if (session.flashTimer)
    {
        m_timerService.cancel(session.flashTimer);
    }

    m_gangZoneService.flashZone(player, zoneId, FLASH_COLOUR, true);
    session.flashTimer = m_timerService.setPlayerTimeout(
        player, FLASH_DURATION,
        [this, zoneId](IPlayer &p) { m_gangZoneService.flashZone(p, zoneId, FLASH_COLOUR, false); });
}

void GangZoneEditorSystem::stretchNearestCorner(IPlayer &player, int zoneId)
{
    GangZoneService::ZoneInfo info;
    if (!m_gangZoneService.getZone(zoneId, info))
    {
        return;
    }

    const Vector3 pos = m_locationService.getPosition(player.getID());
    const Vector2 here(pos.x, pos.y);
    const GangZonePos &r = info.rect;

    // Четыре угла и противоположные им: ближайший к игроку угол тянем к нему,
    // противоположный остаётся на месте.
    const Vector2 corners[4] = {{r.min.x, r.min.y}, {r.min.x, r.max.y}, {r.max.x, r.min.y}, {r.max.x, r.max.y}};
    const Vector2 opposite[4] = {{r.max.x, r.max.y}, {r.max.x, r.min.y}, {r.min.x, r.max.y}, {r.min.x, r.min.y}};

    int nearest = 0;
    float bestDist = std::numeric_limits<float>::max();
    for (int i = 0; i < 4; ++i)
    {
        const float dx = corners[i].x - here.x;
        const float dy = corners[i].y - here.y;
        const float dist = dx * dx + dy * dy;
        if (dist < bestDist)
        {
            bestDist = dist;
            nearest = i;
        }
    }

    m_gangZoneService.setZoneRect(zoneId, here, opposite[nearest]);
}

void GangZoneEditorSystem::moveZoneCenterHere(IPlayer &player, int zoneId)
{
    GangZoneService::ZoneInfo info;
    if (!m_gangZoneService.getZone(zoneId, info))
    {
        return;
    }

    const Vector3 pos = m_locationService.getPosition(player.getID());
    const float halfX = (info.rect.max.x - info.rect.min.x) * 0.5f;
    const float halfY = (info.rect.max.y - info.rect.min.y) * 0.5f;
    m_gangZoneService.setZoneRect(zoneId, Vector2(pos.x - halfX, pos.y - halfY),
                                  Vector2(pos.x + halfX, pos.y + halfY));
}

// ------------------------------------------------------------------ экраны диалогов

void GangZoneEditorSystem::showMain(IPlayer &player)
{
    const std::size_t count = m_gangZoneService.listZones().size();

    std::string body;
    body += fmt::format("Создать зону здесь ({:.0f}x{:.0f})\n", DEFAULT_ZONE_SIZE, DEFAULT_ZONE_SIZE);
    body += fmt::format("Список зон ({})\n", count);
    body += "Сохранить в файл\n";
    body += "Загрузить из файла\n";
    body += "Удалить все зоны";

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Редактор ганг-зон", body, "Выбрать", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = onlinePlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 return;
                             }

                             switch (listItem)
                             {
                             case 0:
                                 createZoneAt(*player);
                                 break;
                             case 1:
                                 showZoneList(*player);
                                 break;
                             case 2:
                                 showSaveNameInput(*player);
                                 break;
                             case 3:
                                 listZoneFilesAsync(*player); // листинг каталога на воркере
                                 break;
                             case 4:
                                 showClearConfirm(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void GangZoneEditorSystem::showZoneList(IPlayer &player)
{
    std::vector<GangZoneService::ZoneInfo> zones = m_gangZoneService.listZones();

    std::vector<int> mapping;
    std::string body;
    for (const GangZoneService::ZoneInfo &zone : zones)
    {
        body += fmt::format("#{}\t({:.0f} {:.0f})-({:.0f} {:.0f})\t{}\tчастей: {}\n", zone.id, zone.rect.min.x,
                            zone.rect.min.y, zone.rect.max.x, zone.rect.max.y, colourToHex(zone.colour),
                            zone.pieceCount);
        mapping.push_back(zone.id);
    }
    if (body.empty())
    {
        body = "Зон нет";
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Ганг-зоны (по приоритету)", body, "Выбрать", "Назад"),
                         [this, playerId = player.getID(), mapping = std::move(mapping)](DialogResponse response,
                                                                                         int listItem, StringView)
                         {
                             IPlayer *player = onlinePlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)mapping.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             m_sessions[playerId].selectedId = mapping[listItem];
                             highlightZone(*player, mapping[listItem]); // сразу мигнуть — видно, что выбрали
                             showZoneEdit(*player);
                         });
}

void GangZoneEditorSystem::showZoneEdit(IPlayer &player)
{
    Session &session = sessionOf(player);
    GangZoneService::ZoneInfo info;
    if (!m_gangZoneService.getZone(session.selectedId, info))
    {
        session.selectedId = -1;
        showMain(player);
        return;
    }

    std::string body;
    body += "Подсветить (мигание)\n";
    body += "Двигать клавишами\n";
    body += "Менять размер клавишами\n";
    body += fmt::format("Шаг клавиш: {:.1f}\n", sessionOf(player).keyStep);
    body += fmt::format("Цвет: {}\n", colourToHex(info.colour));
    body += "Растянуть ближайший угол до меня\n";
    body += "Переместить зону сюда (центром)\n";
    body += fmt::format("Задать прямоугольник ({:.0f} {:.0f} {:.0f} {:.0f})\n", info.rect.min.x, info.rect.min.y,
                        info.rect.max.x, info.rect.max.y);
    body += "Приоритет выше (обрезает соседей)\n";
    body += "Приоритет ниже (обрезается соседями)\n";
    body += "Удалить";

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_LIST, fmt::format("Зона #{} (частей: {})", info.id, info.pieceCount), body, "Выбрать",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            Session &session = m_sessions[playerId];
            if (response == DialogResponse_Right || session.selectedId < 0)
            {
                showZoneList(*player);
                return;
            }
            const int zoneId = session.selectedId;

            switch (listItem)
            {
            case 0:
                highlightZone(*player, zoneId);
                showZoneEdit(*player);
                break;
            case 1:
                startKeyMode(*player, KeyMode::Move); // диалог закрыт — рулим клавишами, /gzone фиксирует
                break;
            case 2:
                startKeyMode(*player, KeyMode::Resize);
                break;
            case 3:
                showKeyStepInput(*player);
                break;
            case 4:
                showColourInput(*player);
                break;
            case 5:
                stretchNearestCorner(*player, zoneId);
                showZoneEdit(*player);
                break;
            case 6:
                moveZoneCenterHere(*player, zoneId);
                showZoneEdit(*player);
                break;
            case 7:
                showRectInput(*player);
                break;
            case 8:
                if (!m_gangZoneService.moveZonePriority(zoneId, true))
                {
                    player->sendClientMessage(Colour::White(), u("Зона уже с высшим приоритетом"));
                }
                showZoneEdit(*player);
                break;
            case 9:
                if (!m_gangZoneService.moveZonePriority(zoneId, false))
                {
                    player->sendClientMessage(Colour::White(), u("Зона уже с низшим приоритетом"));
                }
                showZoneEdit(*player);
                break;
            case 10:
                m_gangZoneService.removeZone(zoneId);
                session.selectedId = -1;
                player->sendClientMessage(Colour::White(), u("Зона удалена"));
                showZoneList(*player);
                break;
            default:
                break;
            }
        });
}

void GangZoneEditorSystem::showKeyStepInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Шаг клавиш",
                   fmt::format("Введите шаг в метрах за тик ({}-{})", KEY_STEP_MIN, KEY_STEP_MAX), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                try
                {
                    const float step = std::stof(text.to_string());
                    m_sessions[playerId].keyStep = std::clamp(step, KEY_STEP_MIN, KEY_STEP_MAX);
                }
                catch (...)
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showZoneEdit(*player);
        });
}

void GangZoneEditorSystem::showColourInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Цвет зоны",
                   u("Введите цвет: RRGGBB или RRGGBBAA (напр. FF000080 — полупрозрачный красный)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                Colour colour;
                if (!parseHexColour(text.to_string(), colour))
                {
                    player->sendClientMessage(Colour::White(), u("Некорректный цвет. Формат: RRGGBB или RRGGBBAA"));
                    showColourInput(*player);
                    return;
                }
                m_gangZoneService.setZoneColour(m_sessions[playerId].selectedId, colour);
            }

            showZoneEdit(*player);
        });
}

void GangZoneEditorSystem::showRectInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Прямоугольник зоны", "Введите координаты: X1 Y1 X2 Y2", "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
                std::istringstream ss(text.to_string());
                if (!(ss >> x1 >> y1 >> x2 >> y2))
                {
                    player->sendClientMessage(Colour::White(), u("Введите четыре числа: X1 Y1 X2 Y2"));
                    showRectInput(*player);
                    return;
                }
                m_gangZoneService.setZoneRect(m_sessions[playerId].selectedId, Vector2(x1, y1), Vector2(x2, y2));
            }

            showZoneEdit(*player);
        });
}

void GangZoneEditorSystem::showSaveNameInput(IPlayer &player)
{
    if (m_gangZoneService.listZones().empty())
    {
        player.sendClientMessage(Colour::White(), u("Нечего сохранять: зон нет"));
        showMain(player);
        return;
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Сохранить зоны", "Введите имя файла", "Сохранить", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            const std::string name = text.to_string();
            if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                name.find("..") != std::string::npos)
            {
                player->sendClientMessage(Colour::White(), u("Недопустимое имя файла"));
                showSaveNameInput(*player);
                return;
            }

            saveToFileAsync(*player, name); // запись на воркере, сообщение придёт из колбэка
            showMain(*player);
        });
}

void GangZoneEditorSystem::showLoadList(IPlayer &player, std::vector<std::string> files)
{
    std::string body;
    for (const std::string &name : files)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = "Нет сохранённых файлов";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Загрузить зоны (заменяет текущие)", body, "Загрузить", "Назад"),
        [this, playerId = player.getID(), files = std::move(files)](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)files.size())
            {
                showMain(*player);
                return;
            }

            loadFromFileAsync(*player, files[listItem]); // чтение на воркере, итог из колбэка
        });
}

void GangZoneEditorSystem::showClearConfirm(IPlayer &player)
{
    const std::size_t count = m_gangZoneService.listZones().size();
    if (count == 0)
    {
        showMain(player);
        return;
    }

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_MSGBOX, "Удалить все зоны",
                   fmt::format("Удалить все зоны ({})? Несохранённое будет потеряно.", count), "Удалить", "Отмена"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                m_gangZoneService.clearZones();
                m_sessions[playerId].selectedId = -1;
                player->sendClientMessage(Colour::White(), u("Все зоны удалены"));
            }
            showMain(*player);
        });
}

// ------------------------------------------------------------------ файлы
// Диск — на воркерах тредпула; сериализация, парсинг и работа с зонами — на
// главном потоке (SDK не потокобезопасен).

std::string GangZoneEditorSystem::serializeZones() const
{
    std::string out;
    // Строки в порядке приоритета (первая — старшая, обрезает остальных).
    for (const GangZoneService::ZoneInfo &zone : m_gangZoneService.listZones())
    {
        out += fmt::format("zone {:.2f} {:.2f} {:.2f} {:.2f} {}\n", zone.rect.min.x, zone.rect.min.y, zone.rect.max.x,
                           zone.rect.max.y, colourToHex(zone.colour));
    }
    return out;
}

void GangZoneEditorSystem::saveToFileAsync(IPlayer &player, const std::string &name)
{
    const std::string path = ZONES_DIR + "/" + name + ".txt";

    ThreadPool::Task<bool> task;
    task.func = [path, content = serializeZones()]()
    {
        std::error_code ec;
        std::filesystem::create_directories(ZONES_DIR, ec);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error("не удалось открыть файл " + path);
        }
        out << content;
        if (!out.good())
        {
            throw std::runtime_error("ошибка записи " + path);
        }
        return true;
    };
    task.callback = [this, playerId = player.getID(), path](bool)
    {
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Зоны сохранены: " + path));
        }
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
        }
    };
    ThreadPool::addTask(std::move(task));
}

void GangZoneEditorSystem::loadFromFileAsync(IPlayer &player, const std::string &name)
{
    const std::string path = ZONES_DIR + "/" + name + ".txt";

    ThreadPool::Task<std::string> task;
    task.func = [path]()
    {
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("не удалось открыть файл " + path);
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    task.callback = [this, playerId = player.getID(), name](std::string content)
    {
        // Зоны глобальные — применяем независимо от инициатора; очистка старого
        // набора происходит только после успешного чтения файла.
        const std::size_t loaded = loadFromContent(content);

        IPlayer *player = onlinePlayer(playerId);
        if (!player)
        {
            return;
        }
        if (loaded > 0)
        {
            player->sendClientMessage(Colour::White(), u(fmt::format("Загружено зон: {} из {}", loaded, name)));
        }
        else
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: в файле нет валидных зон"));
        }
        showMain(*player);
    };
    task.errorCallback = [this, playerId = player.getID()](const std::string &error)
    {
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            showMain(*player);
        }
    };
    ThreadPool::addTask(std::move(task));
}

void GangZoneEditorSystem::listZoneFilesAsync(IPlayer &player)
{
    ThreadPool::Task<std::vector<std::string>> task;
    task.func = []()
    {
        std::vector<std::string> result;
        std::error_code ec;
        if (!std::filesystem::exists(ZONES_DIR, ec))
        {
            return result;
        }
        for (const auto &entry : std::filesystem::directory_iterator(ZONES_DIR, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".txt")
            {
                result.push_back(entry.path().stem().string());
            }
        }
        return result;
    };
    task.callback = [this, playerId = player.getID()](std::vector<std::string> files)
    {
        if (IPlayer *player = onlinePlayer(playerId))
        {
            showLoadList(*player, std::move(files));
        }
    };
    ThreadPool::addTask(std::move(task));
}

std::size_t GangZoneEditorSystem::loadFromContent(const std::string &content)
{
    // Загрузка заменяет текущий набор: порядок строк восстанавливает приоритеты.
    m_gangZoneService.clearZones();
    std::size_t loaded = 0;

    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind != "zone")
        {
            continue;
        }

        float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
        std::string colourHex;
        if (!(ss >> x1 >> y1 >> x2 >> y2 >> colourHex))
        {
            continue;
        }

        Colour colour;
        if (!parseHexColour(colourHex, colour))
        {
            continue;
        }

        if (m_gangZoneService.addZone(Vector2(x1, y1), Vector2(x2, y2), colour) >= 0)
        {
            ++loaded;
        }
    }

    return loaded;
}
