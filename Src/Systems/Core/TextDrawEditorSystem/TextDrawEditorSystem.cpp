#include "Systems/Core/TextDrawEditorSystem/TextDrawEditorSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

namespace
{
const std::string TEXTDRAWS_DIR = "textdraws";
constexpr uint32_t KEY_SPRINT = 8;          // ускоренное перемещение textdraw
constexpr float MOVE_STEP_MIN = 0.1f;
constexpr float MOVE_STEP_MAX = 20.0f;
constexpr float SPRINT_MULTIPLIER = 5.0f;
constexpr std::size_t LIST_TEXT_PREVIEW = 24;  // байт текста в строке списка
constexpr std::size_t MAX_FILE_ITEMS = 256;    // защита от мусорных файлов
const Colour PICK_HIGHLIGHT = Colour(255, 64, 64, 255);

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// body передаётся уже в cp1251 (через u() по месту сборки): в него попадает
// сырой текст textdraw, введённый игроком, который нельзя прогонять через
// utf8Tocp1251 повторно.
Dialog makeDialog(DialogStyle style, const std::string &title, std::string body, const std::string &leftButton,
                  const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = std::move(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}

bool parseInt(StringView text, int &out)
{
    try
    {
        std::size_t pos = 0;
        out = std::stoi(text.to_string(), &pos);
        return pos > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool parseFloat(const std::string &text, float &out)
{
    try
    {
        std::size_t pos = 0;
        out = std::stof(text, &pos);
        return pos > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool parseVec2(const std::string &text, Vector2 &out)
{
    std::istringstream ss(text);
    return static_cast<bool>(ss >> out.x >> out.y);
}

bool parseVec3(const std::string &text, Vector3 &out)
{
    std::istringstream ss(text);
    return static_cast<bool>(ss >> out.x >> out.y >> out.z);
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
        value = (value << 8) | 0xFF;
    }
    out = Colour::FromRGBA(value);
    return true;
}

std::string colourToHex(Colour colour)
{
    return fmt::format("{:08X}", colour.RGBA());
}

std::string styleName(TextDrawStyle style)
{
    switch (style)
    {
    case TextDrawStyle_0:
        return "0 (Beckett)";
    case TextDrawStyle_1:
        return "1 (Aharoni)";
    case TextDrawStyle_2:
        return "2 (BankGothic)";
    case TextDrawStyle_3:
        return "3 (Pricedown)";
    case TextDrawStyle_Sprite:
        return "спрайт TXD";
    case TextDrawStyle_Preview:
        return "превью модели";
    default:
        return "?";
    }
}

std::string alignmentName(TextDrawAlignmentTypes alignment)
{
    switch (alignment)
    {
    case TextDrawAlignment_Left:
        return "слева";
    case TextDrawAlignment_Center:
        return "по центру";
    case TextDrawAlignment_Right:
        return "справа";
    default:
        return "по умолчанию";
    }
}
} // namespace

TextDrawEditorSystem::TextDrawEditorSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_textDrawService(serviceRegister.getService<TextDrawService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerUpdateDispatcher().addEventHandler(this);

    m_commandService.add("td", {},
                         [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                         {
                             Session &session = sessionOf(player);

                             if (!session.enabled)
                             {
                                 enableEditor(player);
                                 showMain(player);
                                 return;
                             }

                             // В режиме перемещения /td фиксирует textdraw и возвращает в его меню.
                             if (session.moveMode)
                             {
                                 stopMoveMode(player);
                                 showEdit(player);
                             }
                             else
                             {
                                 showMain(player);
                             }
                         });
}

TextDrawEditorSystem::Session &TextDrawEditorSystem::sessionOf(const IPlayer &player)
{
    return m_sessions[player.getID()];
}

IPlayer *TextDrawEditorSystem::editorPlayer(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player || !m_sessions[playerId].enabled)
    {
        return nullptr;
    }
    return player;
}

void TextDrawEditorSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    // Per-player текстдравы умирают вместе с пулом игрока, клик-хендлеры чистит
    // TextDrawSystem — сбрасываем только сессию редактора.
    sessionOf(player) = Session{};
}

// ------------------------------------------------------------------ режимы

void TextDrawEditorSystem::enableEditor(IPlayer &player)
{
    if (!m_textDrawService.isAvailable())
    {
        player.sendClientMessage(Colour::White(), u("Компонент textdraw недоступен"));
        return;
    }

    sessionOf(player).enabled = true;
    player.sendClientMessage(Colour::White(), u("Редактор textdraw включён. /td — открыть меню."));
}

void TextDrawEditorSystem::disableEditor(IPlayer &player)
{
    stopMoveMode(player);
    stopPicking(player);
    deleteAllItems(player);

    Session &session = sessionOf(player);
    session = Session{};
    player.sendClientMessage(Colour::White(), u("Редактор textdraw выключен."));
}

void TextDrawEditorSystem::startMoveMode(IPlayer &player)
{
    // Замораживать игрока нельзя: у замороженного клиента синк клавиш падает до
    // ~1 Гц и движение становится ступенчатым. Персонаж будет бегать — это цена
    // плавного перемещения textdraw.
    Session &session = sessionOf(player);
    session.moveMode = true;
    player.sendClientMessage(Colour::White(),
                             u("Двигайте textdraw стрелками/WASD, Shift — быстрее. /td — зафиксировать."));
}

void TextDrawEditorSystem::stopMoveMode(IPlayer &player)
{
    Session &session = sessionOf(player);
    session.moveMode = false;
}

void TextDrawEditorSystem::startPicking(IPlayer &player)
{
    Session &session = sessionOf(player);
    if (session.items.empty())
    {
        player.sendClientMessage(Colour::White(), u("Нет текстдравов для выбора"));
        showMain(player);
        return;
    }

    // На время выбора все текстдравы должны быть кликабельны.
    for (const Item &item : session.items)
    {
        if (IPlayerTextDraw *textDraw = m_textDrawService.getForPlayer(player, item.textDrawId))
        {
            textDraw->setSelectable(true);
            textDraw->restream();
        }
    }

    session.picking = true;
    m_textDrawService.beginSelection(player, PICK_HIGHLIGHT,
                                     [this, playerId = player.getID()](IPlayer &cancelled)
                                     {
                                         if (IPlayer *player = editorPlayer(playerId))
                                         {
                                             stopPicking(*player);
                                             showMain(*player);
                                         }
                                     });
    player.sendClientMessage(Colour::White(), u("Кликните по textdraw. ESC — отмена."));
}

void TextDrawEditorSystem::stopPicking(IPlayer &player)
{
    Session &session = sessionOf(player);
    if (!session.picking)
    {
        return;
    }
    session.picking = false;
    m_textDrawService.endSelection(player);

    // Возвращаем selectable в выбранные пользователем значения.
    for (const Item &item : session.items)
    {
        if (IPlayerTextDraw *textDraw = m_textDrawService.getForPlayer(player, item.textDrawId))
        {
            textDraw->setSelectable(item.selectable);
            textDraw->restream();
        }
    }
}

// ------------------------------------------------------------------ per-tick

bool TextDrawEditorSystem::onPlayerUpdate(IPlayer &player, TimePoint now)
{
    Session &session = sessionOf(player);
    if (!session.enabled || !session.moveMode)
    {
        return true;
    }

    IPlayerTextDraw *textDraw = selectedTextDraw(player);
    if (!textDraw)
    {
        stopMoveMode(player);
        return true;
    }

    const PlayerKeyData &keys = player.getKeyData();
    const float step = session.moveStep * ((keys.keys & KEY_SPRINT) ? SPRINT_MULTIPLIER : 1.0f);

    Vector2 position = textDraw->getPosition();
    bool changed = false;

    if (keys.upDown < 0)
    {
        position.y -= step;
        changed = true;
    }
    else if (keys.upDown > 0)
    {
        position.y += step;
        changed = true;
    }
    if (keys.leftRight > 0)
    {
        position.x += step;
        changed = true;
    }
    else if (keys.leftRight < 0)
    {
        position.x -= step;
        changed = true;
    }

    if (changed)
    {
        textDraw->setPosition(TextDrawService::clampPosition(position));
        textDraw->restream();
    }

    return true;
}

// ------------------------------------------------------------------ textdraw

int TextDrawEditorSystem::createItem(IPlayer &player, Vector2 position, StringView text, const TextDrawParams &params)
{
    IPlayerTextDraw *textDraw = m_textDrawService.createForPlayer(player, position, text, params);
    if (!textDraw)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось создать textdraw (лимит пула?)"));
        return -1;
    }

    const int textDrawId = textDraw->getID();
    textDraw->show();

    // Клик нужен только режиму «выбрать кликом»: находим элемент по id и открываем его меню.
    m_textDrawService.setPlayerClickHandler(
        player, textDrawId,
        [this, playerId = player.getID(), textDrawId](IPlayer &)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }
            Session &session = m_sessions[playerId];
            if (!session.picking)
            {
                return;
            }
            const int index = indexOfTextDraw(session, textDrawId);
            if (index < 0)
            {
                return;
            }
            session.selected = index;
            stopPicking(*player);
            showEdit(*player);
        });

    Session &session = sessionOf(player);
    session.items.push_back(Item{textDrawId, params.selectable});
    return static_cast<int>(session.items.size()) - 1;
}

void TextDrawEditorSystem::deleteItem(IPlayer &player, int index)
{
    Session &session = sessionOf(player);
    if (index < 0 || index >= (int)session.items.size())
    {
        return;
    }

    m_textDrawService.clearPlayerClickHandler(player, session.items[index].textDrawId);
    m_textDrawService.destroyForPlayer(player, session.items[index].textDrawId);
    session.items.erase(session.items.begin() + index);
    session.selected = -1;
    stopMoveMode(player);
}

void TextDrawEditorSystem::deleteAllItems(IPlayer &player)
{
    Session &session = sessionOf(player);
    for (const Item &item : session.items)
    {
        m_textDrawService.clearPlayerClickHandler(player, item.textDrawId);
        m_textDrawService.destroyForPlayer(player, item.textDrawId);
    }
    session.items.clear();
    session.selected = -1;
    stopMoveMode(player);
}

IPlayerTextDraw *TextDrawEditorSystem::itemTextDraw(IPlayer &player, int index)
{
    Session &session = sessionOf(player);
    if (index < 0 || index >= (int)session.items.size())
    {
        return nullptr;
    }
    return m_textDrawService.getForPlayer(player, session.items[index].textDrawId);
}

IPlayerTextDraw *TextDrawEditorSystem::selectedTextDraw(IPlayer &player)
{
    Session &session = sessionOf(player);
    IPlayerTextDraw *textDraw = itemTextDraw(player, session.selected);
    if (!textDraw)
    {
        session.selected = -1;
    }
    return textDraw;
}

int TextDrawEditorSystem::indexOfTextDraw(const Session &session, int textDrawId) const
{
    for (std::size_t i = 0; i < session.items.size(); ++i)
    {
        if (session.items[i].textDrawId == textDrawId)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ------------------------------------------------------------------ экраны диалогов

void TextDrawEditorSystem::showMain(IPlayer &player)
{
    Session &session = sessionOf(player);

    std::string body;
    body += u("Создать текстовый textdraw\n");
    body += u("Создать превью модели\n");
    body += u(fmt::format("Список текстдравов ({})\n", session.items.size()));
    body += u("Выбрать кликом мыши\n");
    body += u("Сохранить в файл\n");
    body += u("Загрузить из файла\n");
    body += u("Удалить все текстдравы\n");
    body += u("Выйти из редактора");

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Редактор textdraw", std::move(body), "Выбрать", "Закрыть"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 return; // просто закрыли меню
                             }

                             switch (listItem)
                             {
                             case 0:
                                 showCreateTextInput(*player);
                                 break;
                             case 1:
                                 showCreateModelInput(*player);
                                 break;
                             case 2:
                                 showList(*player);
                                 break;
                             case 3:
                                 startPicking(*player);
                                 break;
                             case 4:
                                 showSaveNameInput(*player);
                                 break;
                             case 5:
                                 showLoadList(*player);
                                 break;
                             case 6:
                                 deleteAllItems(*player);
                                 player->sendClientMessage(Colour::White(), u("Все текстдравы удалены"));
                                 showMain(*player);
                                 break;
                             case 7:
                                 disableEditor(*player);
                                 break;
                             default:
                                 break;
                             }
                         });
}

void TextDrawEditorSystem::showCreateTextInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Создать textdraw",
                   u("Введите текст (коды вида ~r~, ~n~ поддерживаются)"), "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            std::string error;
            if (!TextDrawService::validateText(text, error))
            {
                player->sendClientMessage(Colour::White(), u("Некорректный текст: " + error));
                showCreateTextInput(*player);
                return;
            }

            Session &session = m_sessions[playerId];
            const int index = createItem(*player, Vector2(320.0f, 240.0f), text, TextDrawParams{});
            if (index >= 0)
            {
                session.selected = index;
                showEdit(*player);
            }
            else
            {
                showMain(*player);
            }
        });
}

void TextDrawEditorSystem::showCreateModelInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Превью модели", u("Введите ID модели (скин, машина, объект)"), "Создать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Right)
            {
                showMain(*player);
                return;
            }

            int model = 0;
            if (!parseInt(text, model))
            {
                player->sendClientMessage(Colour::White(), u("Введите корректный числовой ID модели"));
                showCreateModelInput(*player);
                return;
            }

            TextDrawParams params;
            params.style = TextDrawStyle_Preview;
            params.previewModel = model;
            params.box = true;
            params.boxColour = Colour(0, 0, 0, 128);
            params.textSize = Vector2(64.0f, 64.0f);

            Session &session = m_sessions[playerId];
            const int index = createItem(*player, Vector2(320.0f, 240.0f), "preview", params);
            if (index >= 0)
            {
                session.selected = index;
                showEdit(*player);
            }
            else
            {
                showMain(*player);
            }
        });
}

void TextDrawEditorSystem::showList(IPlayer &player)
{
    Session &session = sessionOf(player);

    std::string body;
    for (std::size_t i = 0; i < session.items.size(); ++i)
    {
        IPlayerTextDraw *textDraw = m_textDrawService.getForPlayer(player, session.items[i].textDrawId);
        if (!textDraw)
        {
            body += u(fmt::format("#{}\t(удалён)\n", i));
            continue;
        }
        const Vector2 pos = textDraw->getPosition();
        // Текст уже в cp1251 — добавляем как есть, без повторной конвертации.
        std::string preview = textDraw->getText().to_string().substr(0, LIST_TEXT_PREVIEW);
        body += u(fmt::format("#{}\t{:.0f} {:.0f}\t", i, pos.x, pos.y)) + preview + "\n";
    }
    if (body.empty())
    {
        body = u("Список пуст");
    }

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Текстдравы", std::move(body), "Выбрать", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             Session &session = m_sessions[playerId];
                             if (response != DialogResponse_Left || listItem < 0 ||
                                 listItem >= (int)session.items.size())
                             {
                                 showMain(*player);
                                 return;
                             }

                             session.selected = listItem;
                             showEdit(*player);
                         });
}

void TextDrawEditorSystem::showEdit(IPlayer &player)
{
    Session &session = sessionOf(player);
    IPlayerTextDraw *textDraw = selectedTextDraw(player);
    if (!textDraw)
    {
        showMain(player);
        return;
    }

    const Vector2 pos = textDraw->getPosition();
    const Vector2 letterSize = textDraw->getLetterSize();
    const Vector2 textSize = textDraw->getTextSize();
    const Vector3 previewRot = textDraw->getPreviewRotation();
    const Item &item = session.items[session.selected];

    std::string body;
    body += u("Текст: ") + textDraw->getText().to_string().substr(0, LIST_TEXT_PREVIEW * 2) + "\n";
    body += u(fmt::format("Позиция: {:.1f} {:.1f}\n", pos.x, pos.y));
    body += u("Двигать клавишами\n");
    body += u(fmt::format("Шаг перемещения: {:.1f}\n", session.moveStep));
    body += u(fmt::format("Стиль: {}\n", styleName(textDraw->getStyle())));
    body += u(fmt::format("Размер букв: {:.3f} {:.3f}\n", letterSize.x, letterSize.y));
    body += u(fmt::format("Размер текста (бокса): {:.1f} {:.1f}\n", textSize.x, textSize.y));
    body += u(fmt::format("Выравнивание: {}\n", alignmentName(textDraw->getAlignment())));
    body += u(fmt::format("Цвет текста: {}\n", colourToHex(textDraw->getLetterColour())));
    body += u(fmt::format("Бокс: {}\n", textDraw->hasBox() ? "ВКЛ" : "выкл"));
    body += u(fmt::format("Цвет бокса: {}\n", colourToHex(textDraw->getBoxColour())));
    body += u(fmt::format("Цвет фона/тени: {}\n", colourToHex(textDraw->getBackgroundColour())));
    body += u(fmt::format("Тень: {}\n", textDraw->getShadow()));
    body += u(fmt::format("Контур: {}\n", textDraw->getOutline()));
    body += u(fmt::format("Пропорциональный: {}\n", textDraw->isProportional() ? "да" : "нет"));
    body += u(fmt::format("Кликабельный: {}\n", item.selectable ? "да" : "нет"));
    body += u(fmt::format("Превью: модель {}\n", textDraw->getPreviewModel()));
    body += u(fmt::format("Превью: поворот {:.1f} {:.1f} {:.1f}\n", previewRot.x, previewRot.y, previewRot.z));
    body += u(fmt::format("Превью: зум {:.2f}\n", textDraw->getPreviewZoom()));
    body += u("Дублировать\n");
    body += u("Удалить");

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, fmt::format("Textdraw #{}", session.selected), std::move(body), "Выбрать", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            Session &session = m_sessions[playerId];
            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Right || !textDraw)
            {
                showMain(*player);
                return;
            }

            switch (listItem)
            {
            case 0:
                showTextInput(*player);
                break;
            case 1:
                showPositionInput(*player);
                break;
            case 2:
                startMoveMode(*player); // диалог закрыт — двигаем клавишами, /td фиксирует
                break;
            case 3:
                showMoveStepInput(*player);
                break;
            case 4:
                showStylePicker(*player);
                break;
            case 5:
                showLetterSizeInput(*player);
                break;
            case 6:
                showTextSizeInput(*player);
                break;
            case 7:
                showAlignmentPicker(*player);
                break;
            case 8:
                showColourInput(*player, ColourTarget::Letter);
                break;
            case 9:
                textDraw->useBox(!textDraw->hasBox());
                textDraw->restream();
                showEdit(*player);
                break;
            case 10:
                showColourInput(*player, ColourTarget::Box);
                break;
            case 11:
                showColourInput(*player, ColourTarget::Background);
                break;
            case 12:
                showShadowInput(*player);
                break;
            case 13:
                showOutlineInput(*player);
                break;
            case 14:
                textDraw->setProportional(!textDraw->isProportional());
                textDraw->restream();
                showEdit(*player);
                break;
            case 15:
            {
                Item &item = session.items[session.selected];
                item.selectable = !item.selectable;
                textDraw->setSelectable(item.selectable);
                textDraw->restream();
                showEdit(*player);
                break;
            }
            case 16:
                showPreviewModelInput(*player);
                break;
            case 17:
                showPreviewRotationInput(*player);
                break;
            case 18:
                showPreviewZoomInput(*player);
                break;
            case 19: // дублировать
            {
                TextDrawParams params = TextDrawService::readParams(*textDraw);
                params.selectable = session.items[session.selected].selectable;
                const Vector2 pos = textDraw->getPosition() + Vector2(10.0f, 10.0f);
                const int index = createItem(*player, pos, textDraw->getText(), params);
                if (index >= 0)
                {
                    session.selected = index;
                }
                showEdit(*player);
                break;
            }
            case 20:
                deleteItem(*player, session.selected);
                player->sendClientMessage(Colour::White(), u("Textdraw удалён"));
                showMain(*player);
                break;
            default:
                break;
            }
        });
}

void TextDrawEditorSystem::showTextInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Текст", u("Введите новый текст (~n~ — перенос строки)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                std::string error;
                if (!TextDrawService::validateText(text, error))
                {
                    player->sendClientMessage(Colour::White(), u("Некорректный текст: " + error));
                    showTextInput(*player);
                    return;
                }
                textDraw->setText(TextDrawService::sanitizeText(text));
                textDraw->restream();
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showPositionInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Позиция", u("Введите координаты экрана: X Y (экран 640x480)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                Vector2 pos;
                if (parseVec2(text.to_string(), pos))
                {
                    textDraw->setPosition(TextDrawService::clampPosition(pos));
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите два числа: X Y"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showMoveStepInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Шаг перемещения",
                   u(fmt::format("Введите шаг в пикселях за тик ({}-{})", MOVE_STEP_MIN, MOVE_STEP_MAX)), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response == DialogResponse_Left)
            {
                float step = 0.0f;
                if (parseFloat(text.to_string(), step))
                {
                    m_sessions[playerId].moveStep = std::clamp(step, MOVE_STEP_MIN, MOVE_STEP_MAX);
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showStylePicker(IPlayer &player)
{
    std::string body;
    body += u("Шрифт 0 (Beckett)\n");
    body += u("Шрифт 1 (Aharoni)\n");
    body += u("Шрифт 2 (BankGothic)\n");
    body += u("Шрифт 3 (Pricedown)\n");
    body += u("Спрайт TXD (текст — имя спрайта, напр. LD_BEAT:chit)\n");
    body += u("Превью модели");

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Стиль textdraw", std::move(body), "Выбрать", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             IPlayerTextDraw *textDraw = selectedTextDraw(*player);
                             if (response == DialogResponse_Left && textDraw && listItem >= 0 && listItem <= 5)
                             {
                                 textDraw->setStyle(static_cast<TextDrawStyle>(listItem));
                                 textDraw->restream();
                             }

                             showEdit(*player);
                         });
}

void TextDrawEditorSystem::showLetterSizeInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Размер букв", u("Введите ширину и высоту букв: X Y (напр. 0.4 1.6)"), "OK",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                Vector2 size;
                if (parseVec2(text.to_string(), size))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.letterSize = size;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите два числа: X Y"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showTextSizeInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Размер текста",
                   u("Введите размер области текста/бокса: X Y (для спрайта и превью — размер картинки)"), "OK",
                   "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                Vector2 size;
                if (parseVec2(text.to_string(), size))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.textSize = size;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите два числа: X Y"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showAlignmentPicker(IPlayer &player)
{
    std::string body;
    body += u("По умолчанию\n");
    body += u("Слева\n");
    body += u("По центру\n");
    body += u("Справа");

    m_dialogService.show(player, makeDialog(DialogStyle_LIST, "Выравнивание", std::move(body), "Выбрать", "Назад"),
                         [this, playerId = player.getID()](DialogResponse response, int listItem, StringView)
                         {
                             IPlayer *player = editorPlayer(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             IPlayerTextDraw *textDraw = selectedTextDraw(*player);
                             if (response == DialogResponse_Left && textDraw && listItem >= 0 && listItem <= 3)
                             {
                                 textDraw->setAlignment(static_cast<TextDrawAlignmentTypes>(listItem));
                                 textDraw->restream();
                             }

                             showEdit(*player);
                         });
}

void TextDrawEditorSystem::showColourInput(IPlayer &player, ColourTarget target)
{
    const char *title = target == ColourTarget::Letter  ? "Цвет текста"
                        : target == ColourTarget::Box   ? "Цвет бокса"
                                                        : "Цвет фона/тени";

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, title, u("Введите цвет: RRGGBB или RRGGBBAA (напр. FF0000 или FF000080)"), "OK",
                   "Назад"),
        [this, playerId = player.getID(), target](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                Colour colour;
                if (parseHexColour(text.to_string(), colour))
                {
                    switch (target)
                    {
                    case ColourTarget::Letter:
                        textDraw->setColour(colour);
                        break;
                    case ColourTarget::Box:
                        textDraw->setBoxColour(colour);
                        break;
                    case ColourTarget::Background:
                        textDraw->setBackgroundColour(colour);
                        break;
                    }
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Некорректный цвет. Формат: RRGGBB или RRGGBBAA"));
                    showColourInput(*player, target);
                    return;
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showShadowInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Тень", u("Введите размер тени (0 — без тени)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                int shadow = 0;
                if (parseInt(text, shadow))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.shadow = shadow;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите целое число"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showOutlineInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Контур", u("Введите толщину контура (0 — без контура)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                int outline = 0;
                if (parseInt(text, outline))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.outline = outline;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите целое число"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showPreviewModelInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Превью — модель", u("Введите ID модели (работает при стиле «превью модели»)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                int model = 0;
                if (parseInt(text, model))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.previewModel = model;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите целое число"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showPreviewRotationInput(IPlayer &player)
{
    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, "Превью — поворот", u("Введите углы поворота модели: X Y Z (напр. -10 0 -20)"),
                   "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                Vector3 rotation;
                if (parseVec3(text.to_string(), rotation))
                {
                    textDraw->setPreviewRotation(rotation);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите три числа: X Y Z"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showPreviewZoomInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Превью — зум", u("Введите зум камеры превью (напр. 1.0)"), "OK", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            IPlayerTextDraw *textDraw = selectedTextDraw(*player);
            if (response == DialogResponse_Left && textDraw)
            {
                float zoom = 0.0f;
                if (parseFloat(text.to_string(), zoom))
                {
                    TextDrawParams params = TextDrawService::readParams(*textDraw);
                    params.previewZoom = zoom;
                    m_textDrawService.applyParams(*textDraw, params);
                    textDraw->restream();
                }
                else
                {
                    player->sendClientMessage(Colour::White(), u("Введите корректное число"));
                }
            }

            showEdit(*player);
        });
}

void TextDrawEditorSystem::showSaveNameInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Сохранить текстдравы", u("Введите имя файла"), "Сохранить", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, int, StringView text)
        {
            IPlayer *player = editorPlayer(playerId);
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

            std::string error;
            if (saveToFile(*player, name, error))
            {
                player->sendClientMessage(Colour::White(),
                                          u(fmt::format("Сохранено: {}/{}.txt", TEXTDRAWS_DIR, name)));
            }
            else
            {
                player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
            }
            showMain(*player);
        });
}

void TextDrawEditorSystem::showLoadList(IPlayer &player)
{
    std::vector<std::string> files = listTextDrawFiles();

    std::string body;
    for (const std::string &name : files)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = u("Нет сохранённых файлов");
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Загрузить текстдравы", std::move(body), "Загрузить", "Назад"),
        [this, playerId = player.getID(), files = std::move(files)](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = editorPlayer(playerId);
            if (!player)
            {
                return;
            }

            if (response != DialogResponse_Left || listItem < 0 || listItem >= (int)files.size())
            {
                showMain(*player);
                return;
            }

            std::string error;
            if (loadFromFile(*player, files[listItem], error))
            {
                player->sendClientMessage(Colour::White(), u("Загружено: " + files[listItem]));
            }
            else
            {
                player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            }
            showMain(*player);
        });
}

// ------------------------------------------------------------------ файлы

std::vector<std::string> TextDrawEditorSystem::listTextDrawFiles() const
{
    std::vector<std::string> result;
    std::error_code ec;
    if (!std::filesystem::exists(TEXTDRAWS_DIR, ec))
    {
        return result;
    }
    for (const auto &entry : std::filesystem::directory_iterator(TEXTDRAWS_DIR, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
        {
            result.push_back(entry.path().stem().string());
        }
    }
    return result;
}

bool TextDrawEditorSystem::saveToFile(IPlayer &player, const std::string &name, std::string &error)
{
    std::error_code ec;
    std::filesystem::create_directories(TEXTDRAWS_DIR, ec);

    const std::string path = TEXTDRAWS_DIR + "/" + name + ".txt";
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        error = "не удалось открыть файл " + path;
        return false;
    }

    Session &session = sessionOf(player);
    for (const Item &item : session.items)
    {
        IPlayerTextDraw *textDraw = m_textDrawService.getForPlayer(player, item.textDrawId);
        if (!textDraw)
        {
            continue;
        }

        TextDrawParams p = TextDrawService::readParams(*textDraw);
        p.selectable = item.selectable; // на диске — желаемое значение, не форсированное выбором
        const Vector2 pos = textDraw->getPosition();

        // Текст — последним «жадным» полем до конца строки (может содержать пробелы).
        out << fmt::format("td {:.2f} {:.2f} {} {} {:.4f} {:.4f} {:.2f} {:.2f} {} {} {} {} {} {} {} {} {} {:.2f} "
                           "{:.2f} {:.2f} {:.3f} {} {} {}\n",
                           pos.x, pos.y, (int)p.alignment, (int)p.style, p.letterSize.x, p.letterSize.y, p.textSize.x,
                           p.textSize.y, colourToHex(p.letterColour), colourToHex(p.boxColour),
                           colourToHex(p.backgroundColour), p.box ? 1 : 0, p.proportional ? 1 : 0,
                           p.selectable ? 1 : 0, p.shadow, p.outline, p.previewModel, p.previewRotation.x,
                           p.previewRotation.y, p.previewRotation.z, p.previewZoom, p.previewVehicleColour1,
                           p.previewVehicleColour2, textDraw->getText().to_string());
    }

    return true;
}

bool TextDrawEditorSystem::loadFromFile(IPlayer &player, const std::string &name, std::string &error)
{
    const std::string path = TEXTDRAWS_DIR + "/" + name + ".txt";
    std::ifstream in(path);
    if (!in)
    {
        error = "не удалось открыть файл " + path;
        return false;
    }

    Session &session = sessionOf(player);
    std::size_t loaded = 0;

    std::string line;
    while (std::getline(in, line) && session.items.size() < MAX_FILE_ITEMS)
    {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind != "td")
        {
            continue;
        }

        Vector2 pos;
        int alignment = 0;
        int style = 0;
        TextDrawParams p;
        std::string letterHex, boxHex, backgroundHex;
        int box = 0, proportional = 0, selectable = 0;

        if (!(ss >> pos.x >> pos.y >> alignment >> style >> p.letterSize.x >> p.letterSize.y >> p.textSize.x >>
              p.textSize.y >> letterHex >> boxHex >> backgroundHex >> box >> proportional >> selectable >> p.shadow >>
              p.outline >> p.previewModel >> p.previewRotation.x >> p.previewRotation.y >> p.previewRotation.z >>
              p.previewZoom >> p.previewVehicleColour1 >> p.previewVehicleColour2))
        {
            continue;
        }

        if (!parseHexColour(letterHex, p.letterColour) || !parseHexColour(boxHex, p.boxColour) ||
            !parseHexColour(backgroundHex, p.backgroundColour))
        {
            continue;
        }

        p.alignment = static_cast<TextDrawAlignmentTypes>(alignment);
        p.style = static_cast<TextDrawStyle>(style);
        p.box = box != 0;
        p.proportional = proportional != 0;
        p.selectable = selectable != 0;

        std::string text;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ')
        {
            text.erase(0, 1);
        }

        if (createItem(player, pos, text, p) >= 0)
        {
            ++loaded;
        }
    }

    if (loaded == 0)
    {
        error = "в файле нет валидных текстдравов";
        return false;
    }
    return true;
}
