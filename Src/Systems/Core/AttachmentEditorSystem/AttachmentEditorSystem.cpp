#include "Systems/Core/AttachmentEditorSystem/AttachmentEditorSystem.h"

#include "Services/AdminService/AdminService.h"
#include "ThreadPool/ThreadPool.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include "Utils/FileNameSanitizer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace
{
const std::string PRESETS_DIR = "attachments";
constexpr int MAX_MODEL = AttachmentService::MAX_MODEL; // единый владелец предела — сервис

struct BoneInfo
{
    PlayerBone bone;
    const char *name;
};

// 18 валидных костей (PlayerBone_None непригодна для attach) с человекочитаемыми
// русскими именами для LIST-диалога выбора.
constexpr BoneInfo BONES[] = {
    {PlayerBone_Spine, "Позвоночник"},
    {PlayerBone_Head, "Голова"},
    {PlayerBone_LeftUpperArm, "Левое плечо"},
    {PlayerBone_RightUpperArm, "Правое плечо"},
    {PlayerBone_LeftHand, "Левая кисть"},
    {PlayerBone_RightHand, "Правая кисть"},
    {PlayerBone_LeftThigh, "Левое бедро"},
    {PlayerBone_RightThigh, "Правое бедро"},
    {PlayerBone_LeftFoot, "Левая стопа"},
    {PlayerBone_RightFoot, "Правая стопа"},
    {PlayerBone_RightCalf, "Правая голень"},
    {PlayerBone_LeftCalf, "Левая голень"},
    {PlayerBone_LeftForearm, "Левое предплечье"},
    {PlayerBone_RightForearm, "Правое предплечье"},
    {PlayerBone_LeftShoulder, "Левая ключица"},
    {PlayerBone_RightShoulder, "Правая ключица"},
    {PlayerBone_Neck, "Шея"},
    {PlayerBone_Jaw, "Челюсть"},
};
constexpr int BONE_COUNT = sizeof(BONES) / sizeof(BONES[0]);

std::string boneName(int bone)
{
    for (const BoneInfo &info : BONES)
    {
        if (info.bone == bone)
        {
            return info.name;
        }
    }
    return "?";
}

// Общий whitelist-хелпер (Utils/FileNameSanitizer.h) — единая проверка для всех
// редакторов пресетов.
bool sanitizeFileName(const std::string &name)
{
    return Utils::isValidPresetName(name);
}

// "x y z" (разделитель пробел/запятая) -> вектор. Меньше/больше трёх чисел
// или нечисловой мусор — невалид.
bool parseVector3(const std::string &text, Vector3 &out)
{
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream ss(normalized);
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!(ss >> x >> y >> z))
    {
        return false;
    }
    std::string extra;
    if (ss >> extra)
    {
        return false; // лишний токен после трёх чисел
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        return false;
    }
    out = Vector3(x, y, z);
    return true;
}

std::string vectorLine(const Vector3 &v)
{
    return fmt::format("{:.2f} {:.2f} {:.2f}", v.x, v.y, v.z);
}
} // namespace

AttachmentEditorSystem::AttachmentEditorSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_attachmentService(serviceRegister.getService<AttachmentService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    // Admin-гейт — на диспетче команды (как в прочих дев-редакторах проекта);
    // обработчики диалогов его не перепроверяют. Принятый риск: если дева разжаловали
    // с открытым диалогом, он доиграет действие — но эффект строго на СВОЁМ игроке и
    // косметический (аттач объекта к себе), эскалации/вреда другим нет.
    m_commandService.add(
        "aedit", {}, [this](IPlayer &player, const PlayerCommandService::CommandArgs &) { showMain(player); },
        PermissionSpec::admin(AdminService::DEVELOPER_LEVEL), "дев-меню аттачей (меню)",
        PlayerCommandService::HelpCategory::Hidden);
}

AttachmentEditorSystem::Session &AttachmentEditorSystem::sessionOf(const IPlayer &player)
{
    return m_sessions[player.getID()];
}

IPlayer *AttachmentEditorSystem::onlinePlayer(int playerId)
{
    return m_core.getPlayers().get(playerId);
}

void AttachmentEditorSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    const int playerId = player.getID();
    // Инвалидируем ожидающие async-колбэки этого слота: переподключившийся в тот же
    // id игрок не унаследует чужую загрузку/сохранение.
    ++m_sessionToken[playerId];
    // Сам аттач снимается AttachmentSystem::resetPlayer; тут только своя сессия.
    m_sessions[playerId] = Session{};
}

// ------------------------------------------------------------------ действия

bool AttachmentEditorSystem::ensureSlotValid(IPlayer &player)
{
    Session &session = sessionOf(player);
    if (session.slot < 0)
    {
        return false;
    }
    if (!m_attachmentService.isAttached(player.getID(), session.slot))
    {
        session.slot = -1; // слот сняли извне/дев переподключился в тот же id
        return false;
    }
    return true;
}

bool AttachmentEditorSystem::applyCurrent(IPlayer &player)
{
    Session &session = sessionOf(player);
    if (session.slot < 0)
    {
        return false;
    }
    return m_attachmentService.attachToSlot(player, session.slot, session.model,
                                            static_cast<PlayerBone>(session.bone), session.offset, session.rotation,
                                            session.scale);
}

void AttachmentEditorSystem::beginGizmoEdit(IPlayer &player)
{
    Session &session = sessionOf(player);
    const int slot = session.slot;
    const std::uint64_t token = m_sessionToken[player.getID()];

    const bool started = m_attachmentService.beginEdit(
        player, slot,
        [this, playerId = player.getID(), slot, token](IPlayer &, int editedSlot, bool saved)
        {
            if (m_sessionToken[playerId] != token)
            {
                return; // дев вышел/переподключился, пока шла подгонка
            }
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }

            if (saved && editedSlot == slot)
            {
                // Гизмо кладёт новые offset/rotation/scale прямо в сервис — читаем их
                // обратно, иначе сохранение в файл возьмёт старые числовые значения.
                ObjectAttachmentSlotData data;
                if (m_attachmentService.slotData(playerId, slot, data))
                {
                    Session &session = sessionOf(*player);
                    session.offset = data.offset;
                    session.rotation = data.rotation;
                    session.scale = data.scale;
                }
                player->sendClientMessage(Colour::White(), u("Подгонка сохранена"));
            }
            else
            {
                player->sendClientMessage(Colour::White(), u("Подгонка отменена"));
            }
            showMain(*player);
        });

    if (!started)
    {
        player.sendClientMessage(Colour::White(), u("Не удалось начать подгонку"));
        showMain(player);
        return;
    }

    player.sendClientMessage(Colour::White(), u("Мышь — двигать/крутить/масштаб, клик — сохранить, ESC — отмена."));
}

// ------------------------------------------------------------------ экраны диалогов

void AttachmentEditorSystem::showMain(IPlayer &player)
{
    Session &session = sessionOf(player);
    ensureSlotValid(player); // мог быть снят извне/дев переподключился в тот же слот id

    std::string body;
    body += session.slot >= 0
                ? fmt::format("Приатачить/заменить (модель {}, «{}»)\n", session.model,
                              boneName(session.bone))
                : "Приатачить объект\n";
    // Гизмо — основной способ подгонки, идёт сразу за привязкой, выше числовых
    // полей (точный ввод — вторичный путь).
    body += "Редактировать мышью (гизмо)\n";
    body += fmt::format("Смещение (точный ввод): {}\n", vectorLine(session.offset));
    body += fmt::format("Поворот (точный ввод): {}\n", vectorLine(session.rotation));
    body += fmt::format("Масштаб (точный ввод): {}\n", vectorLine(session.scale));
    body += "Сохранить в файл\n";
    body += "Загрузить из файла\n";
    body += "Снять объект";

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Аттачи — дев-меню", body, "Выбрать", "Закрыть"),
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

            // Гейт на пункты, требующие прикреплённого объекта, — на клике, не на
            // видимости (пункты показаны всегда). Приатачить (0) и загрузить (6) —
            // исключения: их можно вызвать и без текущего аттача.
            const bool needsSlot = listItem >= 1 && listItem <= 7 && listItem != 6;
            if (needsSlot && !ensureSlotValid(*player))
            {
                player->sendClientMessage(Colour::White(), u("Сначала приатачьте объект"));
                showMain(*player);
                return;
            }

            switch (listItem)
            {
            case 0:
                showModelInput(*player);
                break;
            case 1:
                beginGizmoEdit(*player);
                break;
            case 2:
                showVectorInput(*player, VectorKind::Offset);
                break;
            case 3:
                showVectorInput(*player, VectorKind::Rotation);
                break;
            case 4:
                showVectorInput(*player, VectorKind::Scale);
                break;
            case 5:
                showSaveNameInput(*player);
                break;
            case 6:
                listPresetFilesAsync(*player); // листинг каталога на воркере
                break;
            case 7:
            {
                Session &session = sessionOf(*player);
                m_attachmentService.detach(*player, session.slot);
                session = Session{};
                player->sendClientMessage(Colour::White(), u("Объект снят"));
                showMain(*player);
                break;
            }
            default:
                break;
            }
        });
}

void AttachmentEditorSystem::showModelInput(IPlayer &player)
{
    m_dialogService.showNumberInput(
        player,
        makeDialog(DialogStyle_INPUT, "Модель объекта", fmt::format("Введите ID модели объекта от 0 до {}", MAX_MODEL),
                   "Далее", "Назад"),
        [this, playerId = player.getID()](DialogResponse response, std::int64_t value)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMain(*player);
                return;
            }

            if (value < 0 || value > MAX_MODEL)
            {
                player->sendClientMessage(Colour::White(), u(fmt::format("ID модели должен быть от 0 до {}", MAX_MODEL)));
                showModelInput(*player);
                return;
            }

            showBoneList(*player, static_cast<int>(value));
        });
}

void AttachmentEditorSystem::showBoneList(IPlayer &player, int model)
{
    std::string body;
    for (const BoneInfo &info : BONES)
    {
        body += std::string(info.name) + "\n";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Выбор кости", body, "Выбрать", "Назад"),
        [this, playerId = player.getID(), model](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left || listItem < 0 || listItem >= BONE_COUNT)
            {
                showMain(*player);
                return;
            }

            Session &session = sessionOf(*player);
            const PlayerBone bone = BONES[listItem].bone;

            // Один активный слот на редактор: замена снимает старый аттач, иначе
            // повторные клики «Приатачить» исчерпают 10 слотов игрока.
            if (session.slot >= 0)
            {
                m_attachmentService.detach(*player, session.slot);
                session.slot = -1;
            }

            const Vector3 defaultOffset(0.0f);
            const Vector3 defaultRotation(0.0f);
            const Vector3 defaultScale(1.0f);
            const int slot =
                m_attachmentService.attach(*player, model, bone, defaultOffset, defaultRotation, defaultScale);
            if (slot < 0)
            {
                player->sendClientMessage(Colour::White(),
                                          u("Не удалось приатачить: нет свободных слотов или невалидные данные"));
                showMain(*player);
                return;
            }

            session.slot = slot;
            session.model = model;
            session.bone = static_cast<int>(bone);
            session.offset = defaultOffset;
            session.rotation = defaultRotation;
            session.scale = defaultScale;
            // Слот только что создан attach() — заведомо валиден, отдельная
            // ensureSlotValid не нужна. Гизмо — основной способ подгонки, открываем
            // его сразу, минуя меню (без лишнего «Приатачено» — сам гизмо это кадром
            // подтверждает, модель/кость видны в карточке меню после выхода);
            // по завершении beginGizmoEdit сам вернёт в showMain.
            beginGizmoEdit(*player);
        });
}

void AttachmentEditorSystem::showVectorInput(IPlayer &player, VectorKind kind)
{
    Session &session = sessionOf(player);
    std::string title;
    Vector3 current;
    switch (kind)
    {
    case VectorKind::Offset:
        title = "Смещение";
        current = session.offset;
        break;
    case VectorKind::Rotation:
        title = "Поворот";
        current = session.rotation;
        break;
    case VectorKind::Scale:
        title = "Масштаб";
        current = session.scale;
        break;
    }

    m_dialogService.show(
        player,
        makeDialog(DialogStyle_INPUT, title,
                   fmt::format("Точный ввод. Введите X Y Z (пробел/запятая). Сейчас: {}", vectorLine(current)), "OK",
                   "Назад"),
        [this, playerId = player.getID(), kind](DialogResponse response, int, StringView text)
        {
            IPlayer *player = onlinePlayer(playerId);
            if (!player)
            {
                return;
            }
            if (response != DialogResponse_Left)
            {
                showMain(*player);
                return;
            }

            if (!ensureSlotValid(*player))
            {
                player->sendClientMessage(Colour::White(), u("Объект больше не приатачен"));
                showMain(*player);
                return;
            }

            Vector3 parsed;
            if (!parseVector3(text.to_string(), parsed))
            {
                player->sendClientMessage(Colour::White(), u("Введите три числа: X Y Z"));
                showVectorInput(*player, kind);
                return;
            }

            Session &session = sessionOf(*player);
            switch (kind)
            {
            case VectorKind::Offset:
                session.offset = parsed;
                break;
            case VectorKind::Rotation:
                session.rotation = parsed;
                break;
            case VectorKind::Scale:
                session.scale = parsed;
                break;
            }

            // attachToSlot санитизирует (клампит offset/scale) перед применением —
            // отклонить может только рассинхрон модели/кости, которых тут не бывает.
            if (!applyCurrent(*player))
            {
                player->sendClientMessage(Colour::White(), u("Не удалось применить (данные отклонены)"));
            }
            showMain(*player);
        });
}

void AttachmentEditorSystem::showSaveNameInput(IPlayer &player)
{
    m_dialogService.show(
        player, makeDialog(DialogStyle_INPUT, "Сохранить пресет", "Введите имя файла (A-Za-z0-9, _, -)", "Сохранить",
                          "Назад"),
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

            if (!ensureSlotValid(*player))
            {
                player->sendClientMessage(Colour::White(), u("Объект больше не приатачен"));
                showMain(*player);
                return;
            }

            const std::string name = text.to_string();
            if (!sanitizeFileName(name))
            {
                player->sendClientMessage(Colour::White(), u("Недопустимое имя файла (разрешены A-Za-z0-9, _, -)"));
                showSaveNameInput(*player);
                return;
            }

            saveToFileAsync(*player, name); // запись на воркере, сообщение придёт из колбэка
            showMain(*player);
        });
}

void AttachmentEditorSystem::showLoadList(IPlayer &player, std::vector<std::string> files)
{
    std::string body;
    for (const std::string &name : files)
    {
        body += name + "\n";
    }
    if (body.empty())
    {
        body = "Нет сохранённых пресетов";
    }

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Загрузить пресет (заменяет текущий аттач)", body, "Загрузить", "Назад"),
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

// ------------------------------------------------------------------ файлы
// Диск — на воркерах тредпула; JSON-парсинг и применение к аттачу — на главном
// потоке (SDK не потокобезопасен).

std::string AttachmentEditorSystem::serializeSession(const Session &session) const
{
    nlohmann::json root;
    root["model"] = session.model;
    root["bone"] = session.bone;
    root["offset"] = {session.offset.x, session.offset.y, session.offset.z};
    root["rotation"] = {session.rotation.x, session.rotation.y, session.rotation.z};
    root["scale"] = {session.scale.x, session.scale.y, session.scale.z};
    return root.dump(2);
}

bool AttachmentEditorSystem::parsePreset(const std::string &content, Session &out) const
{
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(content);
    }
    catch (...)
    {
        return false; // битый JSON
    }
    if (!root.is_object())
    {
        return false;
    }

    auto readVec3 = [](const nlohmann::json &node, Vector3 &vec) -> bool
    {
        if (!node.is_array() || node.size() != 3)
        {
            return false;
        }
        for (const nlohmann::json &component : node)
        {
            if (!component.is_number())
            {
                return false;
            }
        }
        const float x = node[0].get<float>(), y = node[1].get<float>(), z = node[2].get<float>();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            return false; // напр. 1e400 -> inf; согласованно с числовым вводом
        }
        vec = Vector3(x, y, z);
        return true;
    };

    if (!root.contains("model") || !root["model"].is_number_integer())
    {
        return false;
    }
    const int model = root["model"].get<int>();
    if (model < 0 || model > MAX_MODEL)
    {
        return false;
    }

    if (!root.contains("bone") || !root["bone"].is_number_integer())
    {
        return false;
    }
    const int bone = root["bone"].get<int>();
    if (bone <= PlayerBone_None || bone > PlayerBone_Jaw)
    {
        return false;
    }

    Vector3 offset(0.0f), rotation(0.0f), scale(1.0f);
    if (!root.contains("offset") || !readVec3(root["offset"], offset))
    {
        return false;
    }
    if (!root.contains("rotation") || !readVec3(root["rotation"], rotation))
    {
        return false;
    }
    if (!root.contains("scale") || !readVec3(root["scale"], scale))
    {
        return false;
    }

    out.model = model;
    out.bone = bone;
    out.offset = offset;
    out.rotation = rotation;
    out.scale = scale;
    return true;
}

void AttachmentEditorSystem::saveToFileAsync(IPlayer &player, const std::string &name)
{
    const std::string path = PRESETS_DIR + "/" + name + ".json";
    const std::uint64_t token = m_sessionToken[player.getID()];

    ThreadPool::Task<bool> task;
    task.func = [path, content = serializeSession(sessionOf(player))]()
    {
        std::error_code ec;
        std::filesystem::create_directories(PRESETS_DIR, ec);
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
    task.callback = [this, playerId = player.getID(), token, path](bool)
    {
        if (m_sessionToken[playerId] != token)
        {
            return;
        }
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Пресет сохранён: " + path));
        }
    };
    task.errorCallback = [this, playerId = player.getID(), token](const std::string &error)
    {
        if (m_sessionToken[playerId] != token)
        {
            return;
        }
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка сохранения: " + error));
        }
    };
    ThreadPool::addTask(std::move(task));
}

void AttachmentEditorSystem::loadFromFileAsync(IPlayer &player, const std::string &name)
{
    // Ре-санитизация имени перед построением пути чтения (defense-in-depth: даже
    // при выборе из листинга имя не должно вырваться из PRESETS_DIR).
    if (!sanitizeFileName(name))
    {
        player.sendClientMessage(Colour::White(), u("Недопустимое имя пресета"));
        showMain(player);
        return;
    }
    const std::string path = PRESETS_DIR + "/" + name + ".json";
    const std::uint64_t token = m_sessionToken[player.getID()];

    ThreadPool::Task<std::string> task;
    task.func = [path]()
    {
        constexpr std::uintmax_t MAX_PRESET_BYTES = 64 * 1024; // пресет крошечный; больше — мусор/атака
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        if (!ec && size > MAX_PRESET_BYTES)
        {
            throw std::runtime_error("файл слишком большой: " + path);
        }
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("не удалось открыть файл " + path);
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    task.callback = [this, playerId = player.getID(), token, name](std::string content)
    {
        if (m_sessionToken[playerId] != token)
        {
            return; // дев вышел/переподключился в тот же id, пока читался файл
        }
        IPlayer *player = onlinePlayer(playerId);
        if (!player)
        {
            return; // дев вышел, пока читался файл
        }

        Session parsed;
        if (!parsePreset(content, parsed))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: битый файл " + name));
            showMain(*player);
            return;
        }

        Session &session = sessionOf(*player);
        // Заменяем текущий аттач: старый слот снимаем, чтобы не плодить висящие слоты.
        if (session.slot >= 0)
        {
            m_attachmentService.detach(*player, session.slot);
            session.slot = -1;
        }

        const int slot = m_attachmentService.attach(*player, parsed.model, static_cast<PlayerBone>(parsed.bone),
                                                     parsed.offset, parsed.rotation, parsed.scale);
        if (slot < 0)
        {
            player->sendClientMessage(Colour::White(), u("Не удалось применить пресет (нет свободного слота)"));
            showMain(*player);
            return;
        }

        session.slot = slot;
        session.model = parsed.model;
        session.bone = parsed.bone;
        session.offset = parsed.offset;
        session.rotation = parsed.rotation;
        session.scale = parsed.scale;
        player->sendClientMessage(Colour::White(), u("Загружено: " + name));
        showMain(*player);
    };
    task.errorCallback = [this, playerId = player.getID(), token](const std::string &error)
    {
        if (m_sessionToken[playerId] != token)
        {
            return;
        }
        if (IPlayer *player = onlinePlayer(playerId))
        {
            player->sendClientMessage(Colour::White(), u("Ошибка загрузки: " + error));
            showMain(*player);
        }
    };
    ThreadPool::addTask(std::move(task));
}

void AttachmentEditorSystem::listPresetFilesAsync(IPlayer &player)
{
    const std::uint64_t token = m_sessionToken[player.getID()];
    ThreadPool::Task<std::vector<std::string>> task;
    task.func = []()
    {
        std::vector<std::string> result;
        std::error_code ec;
        if (!std::filesystem::exists(PRESETS_DIR, ec))
        {
            return result;
        }
        for (const auto &entry : std::filesystem::directory_iterator(PRESETS_DIR, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                result.push_back(entry.path().stem().string());
            }
        }
        return result;
    };
    task.callback = [this, playerId = player.getID(), token](std::vector<std::string> files)
    {
        if (m_sessionToken[playerId] != token)
        {
            return;
        }
        if (IPlayer *player = onlinePlayer(playerId))
        {
            showLoadList(*player, std::move(files));
        }
    };
    ThreadPool::addTask(std::move(task));
}
