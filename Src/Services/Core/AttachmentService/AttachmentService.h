#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <Server/Components/Objects/objects.hpp>
#include <array>
#include <functional>

class AttachmentSystem;

// Сервис прикреплённых к игроку объектов (10 слотов клиента): предмет в руке,
// инструмент на работе, аксессуары на теле.
//
//   int slot = m_attachments.attach(player, 2228, PlayerBone_RightHand); // бутылка в руку
//   m_attachments.attach(player, 19161, PlayerBone_Head, offset, rot);   // шляпа
//   m_attachments.detach(player, slot);
//   m_attachments.detachAll(player);                                     // напр., на смерти
//
//   // Нативная подгонка аксессуара игроком (гизмо клиента):
//   m_attachments.beginEdit(player, slot, [](IPlayer &p, int slot, bool saved) { ... });
//
// Сервис — источник правды о выданном (клиент стримит прикреплённое всем
// вокруг). Валидация: модель/кость/масштаб/оффсеты клампятся при выдаче, а
// данные из клиентской подгонки санитизируются ДО применения — гигантский
// масштаб или оффсет на полкарты видны всем игрокам и не должны пройти.
// Подгонка применяется только для слота, редактирование которого начал СЕРВЕР.
class AttachmentService final : public IService
{
    friend AttachmentSystem;

  public:
    static constexpr int MAX_SLOTS = MAX_ATTACHED_OBJECT_SLOTS; // 10
    static constexpr int MAX_MODEL = 19999;                     // предел валидного id модели объекта

    // saved=false — игрок отменил подгонку (ESC), слот вернулся к прежнему виду.
    using EditHandler = std::function<void(IPlayer &, int slot, bool saved)>;

    // Прикрепить в свободный слот. Возвращает слот или -1 (всё занято /
    // невалидные данные). Цвета по умолчанию нулевые — без перекраски.
    int attach(IPlayer &player, int model, PlayerBone bone, const Vector3 &offset = Vector3(0.0f),
               const Vector3 &rotation = Vector3(0.0f), const Vector3 &scale = Vector3(1.0f),
               Colour colour1 = Colour::None(), Colour colour2 = Colour::None());
    // В конкретный слот (заменяет содержимое). false — невалидные данные/слот.
    bool attachToSlot(IPlayer &player, int slot, int model, PlayerBone bone, const Vector3 &offset = Vector3(0.0f),
                      const Vector3 &rotation = Vector3(0.0f), const Vector3 &scale = Vector3(1.0f),
                      Colour colour1 = Colour::None(), Colour colour2 = Colour::None());
    void detach(IPlayer &player, int slot);
    void detachAll(IPlayer &player);
    bool isAttached(int playerId, int slot) const;

    // Прочитать актуальные данные слота (в т.ч. после правки гизмо клиента, которая
    // кладётся напрямую в st.data). false — слот пуст, out не трогается.
    bool slotData(int playerId, int slot, ObjectAttachmentSlotData &out) const;

    // Нативная подгонка слота игроком. false — слот пуст или нет расширения.
    bool beginEdit(IPlayer &player, int slot, EditHandler onDone = nullptr);

  private:
    struct State
    {
        std::array<bool, MAX_SLOTS> used{};
        std::array<ObjectAttachmentSlotData, MAX_SLOTS> data{};
        int editingSlot = -1; // слот активной подгонки (-1 — нет)
        EditHandler onEdit;
    };

    // Вызываются AttachmentSystem.
    void handleEdited(IPlayer &player, int slot, bool saved, const ObjectAttachmentSlotData &data);
    void resetPlayer(int playerId);

    static bool sanitize(ObjectAttachmentSlotData &data); // false — данные не спасти

    std::array<State, MAX_PLAYERS> m_state;
};
