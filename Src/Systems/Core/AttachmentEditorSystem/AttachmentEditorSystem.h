#pragma once

#include "Macro.h"
#include "Services/Core/AttachmentService/AttachmentService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Дев-редактор аттачей: /aedit открывает меню. Приатачивает объект по id
// модели на выбранную кость девелоперу (себе); сразу после привязки открывается
// нативный клиентский гизмо (мышью — offset/rotation/scale читаются обратно из
// AttachmentService после сохранения) — это основной способ подгонки. Числовой
// ввод (X Y Z) остаётся как вторичный путь для точных значений. Пресет
// сохраняется/грузится в attachments/<имя>.json. Один активный слот на сессию
// редактора: повторное «Приатачить» снимает предыдущий, чтобы не исчерпать
// 10 слотов игрока.
class AttachmentEditorSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    AttachmentEditorSystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    enum class VectorKind : uint8_t
    {
        Offset,
        Rotation,
        Scale
    };

    struct Session
    {
        int slot = -1; // редактируемый слот аттача (-1 — нет)
        int model = 0;
        int bone = PlayerBone_None;
        Vector3 offset = Vector3(0.0f);
        Vector3 rotation = Vector3(0.0f);
        Vector3 scale = Vector3(1.0f);
    };

    // --- экраны диалогов ---
    void showMain(IPlayer &player);
    void showModelInput(IPlayer &player);
    void showBoneList(IPlayer &player, int model);
    void showVectorInput(IPlayer &player, VectorKind kind);
    void showSaveNameInput(IPlayer &player);
    void showLoadList(IPlayer &player, std::vector<std::string> files);

    // --- действия ---
    bool ensureSlotValid(IPlayer &player); // ре-валидация: слот ещё занят (иначе сброс Session)
    bool applyCurrent(IPlayer &player);    // переприменить session к слоту через attachToSlot
    void beginGizmoEdit(IPlayer &player);

    // --- файлы (диск — в тредпуле, парсинг/применение — на главном потоке) ---
    std::string serializeSession(const Session &session) const;
    bool parsePreset(const std::string &content, Session &out) const;
    void saveToFileAsync(IPlayer &player, const std::string &name);
    void loadFromFileAsync(IPlayer &player, const std::string &name);
    void listPresetFilesAsync(IPlayer &player);

    Session &sessionOf(const IPlayer &player);
    IPlayer *onlinePlayer(int playerId);

    PlayerDialogService &m_dialogService;
    PlayerCommandService &m_commandService;
    AttachmentService &m_attachmentService;

    std::array<Session, MAX_PLAYERS> m_sessions;

    // Монотонный per-slot токен: ++ на дисконнекте. Захватывается при постановке
    // async-задачи (файловый I/O), сверяется в колбэке — если дев вышел или другой
    // игрок переподключился в тот же id за время чтения/записи диска, устаревший
    // колбэк не трогает чужую сессию и не аттачит чужому (serial-guard, конвенция
    // персиста проекта). onlinePlayer() один этого не ловит: id тот же, игрок другой.
    std::array<std::uint64_t, MAX_PLAYERS> m_sessionToken{};
};
