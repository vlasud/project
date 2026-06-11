#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <Server/Components/Objects/objects.hpp>
#include <array>
#include <functional>

class ObjectEditSystem;

// Нативное редактирование объектов: клиентский редактор со стрелками-осями и
// кольцами вращения (как в MTA) и выбор объекта кликом.
//
//   m_objectEdit.beginEdit(player, object,
//       [](IPlayer &p, IObject &o, ObjectEditResponse r, Vector3 pos, Vector3 rot) {
//           // r == Update — тащит прямо сейчас; Final — сохранил; Cancel — ESC
//       });
//   m_objectEdit.beginSelect(player, [](IPlayer &p, IObject &o, int model, Vector3 pos) { ... });
//   m_objectEdit.end(player);
//
// Валидация (ядро покрывает не всё):
//  * ядро отбрасывает Edit-RPC без серверно начатой сессии, НО проверяет только
//    «игрок что-то редактирует», а не какой объект — клиент в своей сессии может
//    прислать правку с чужим ObjectID. Сервис сверяет id объекта сессии и
//    подделку игнорирует;
//  * ядро объект НЕ двигает — координаты применяет только обработчик, и он же
//    отвечает за допустимость (мебель — в границах дома и т.п.).
class ObjectEditService final : public IService
{
    friend ObjectEditSystem;

  public:
    // position/rotation — заявленный клиентом трансформ объекта.
    using EditHandler = std::function<void(IPlayer &, IObject &, ObjectEditResponse, Vector3, Vector3)>;
    using SelectHandler = std::function<void(IPlayer &, IObject &, int model, Vector3 position)>;

    // Начать редактирование глобального объекта. Один сеанс на игрока
    // (повторный вызов заменяет предыдущий). false — нет расширения объектов.
    bool beginEdit(IPlayer &player, IObject &object, EditHandler handler);
    // Выбор объекта кликом; обработчик одноразовый (после клика снимается).
    bool beginSelect(IPlayer &player, SelectHandler handler);
    // Завершить сеанс редактирования/выбора.
    void end(IPlayer &player);
    bool isEditing(int playerId) const;

  private:
    struct Slot
    {
        int editObjectId = -1; // объект активной сессии (-1 — нет)
        EditHandler editHandler;
        SelectHandler selectHandler;
    };

    // Вызываются ObjectEditSystem.
    void handleEdited(IPlayer &player, IObject &object, ObjectEditResponse response, Vector3 position,
                      Vector3 rotation);
    void handleSelected(IPlayer &player, IObject &object, int model, Vector3 position);
    void resetPlayer(int playerId);

    std::array<Slot, MAX_PLAYERS> m_slots;
};
