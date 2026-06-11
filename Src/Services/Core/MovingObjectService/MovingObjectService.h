#pragma once

#include "Services/IService.h"
#include "player.hpp"
#include <Server/Components/Objects/objects.hpp>
#include <functional>
#include <unordered_map>

class MovingObjectSystem;

// Сервис движущихся объектов — фундамент дверей, ворот, шлагбаумов, лифтов.
//
//   int gate = m_moving.create(980, closedPos, closedRot);
//   m_moving.moveTo(gate, openPos, openRot, 2.0f, [](IObject &o) {
//       // приехали — ворота открыты (можно тут же запустить закрытие)
//   });
//   m_moving.stopMoving(gate);    // остановить на месте
//   m_moving.setTransform(gate, pos, rot); // мгновенно, без анимации
//
// Объекты живут в ГЛОБАЛЬНОМ пуле (движение видно всем синхронно и
// интерполируется ядром) — пул ограничен, для статичного декора используй
// StreamerService. Контракт onArrived: вызывается один раз по прибытии; при
// stopMoving()/destroy()/новом moveTo — не вызывается. Прибытие считает сервер
// (ядро ведёт движение само) — клиентских заявок здесь нет.
class MovingObjectService final : public IService
{
    friend MovingObjectSystem;

  public:
    using ArriveHandler = std::function<void(IObject &)>;

    static constexpr float MIN_SPEED = 0.01f;
    static constexpr float MAX_SPEED = 120.0f; // юнитов в секунду

    // Создать управляемый объект. Возвращает id или -1 (пул полон).
    int create(int model, const Vector3 &position, const Vector3 &rotation, float drawDistance = 0.0f);
    void destroy(int objectId);
    bool exists(int objectId) const;

    // Плавное движение к цели со скоростью speed (юнитов/сек).
    // false — объект не из этого сервиса или уже уничтожен.
    bool moveTo(int objectId, const Vector3 &targetPosition, const Vector3 &targetRotation, float speed,
                ArriveHandler onArrived = nullptr);
    void stopMoving(int objectId); // остановить на месте (onArrived не вызывается)
    bool isMoving(int objectId) const;

    // Мгновенная установка (телепорт объекта, прерывает движение).
    void setTransform(int objectId, const Vector3 &position, const Vector3 &rotation);
    Vector3 getPosition(int objectId) const;

  private:
    struct Def
    {
        ArriveHandler onArrived;
    };

    // Вызываются MovingObjectSystem.
    void initialize(IObjectsComponent *objects);
    void handleMoved(IObject &object); // объект ядра доехал до цели

    IObject *getManaged(int objectId) const; // только объекты этого сервиса

    IObjectsComponent *m_objects = nullptr;
    std::unordered_map<int, Def> m_defs; // ключ — pool id созданных сервисом объектов
};
