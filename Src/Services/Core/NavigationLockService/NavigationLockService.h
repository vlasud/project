#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

// Генеричный лок навигации игрока (Core-инфраструктура, БЕЗ бизнеса внутри). Пока
// лок удержан, GPS-навигация (/gps) игроку недоступна: обычный чекпоинт-слот в это
// время принадлежит бизнесу, взявшему лок (работа водителем/грузчиком, где слот
// занят рабочими маркерами). Сервис о бизнесе ничего не знает — причина лока это
// просто строка (utf-8) от держателя, показываемая игроку в отказе.
//
// ОДИН держатель на игрока: сервис сам вложенности не считает — повторный acquire
// ПЕРЕЗАПИСЫВАЕТ причину, а release снимает лок безусловно. Непересечение смен —
// ответственность держателей: обе работы в onStartWork отказывают устройству при
// уже удержанном локе (взаимное исключение), поэтому лок держит ровно одна смена.
// release/reset идемпотентны. reset — на дисконнекте: переиспользуемый слот не
// наследует чужой лок.
//
// Взятие лока ГАСИТ активный GPS-чекпоинт игрока (иначе GPS-маркер повис бы поверх
// рабочих маркеров). Сам сервис клиентский чекпоинт не трогает (тот принадлежит
// бизнес-слою): acquire лишь оповещает subscribeAcquired, а гасит GPS подписчик
// (GpsSystem). Подписки регистрируются в конструкторах систем — до первого acquire
// (он приходит только в игре, при устройстве на работу).
class NavigationLockService final : public IService
{
  public:
    using AcquireObserver = std::function<void(int playerId)>;

    // Взять лок с причиной (utf-8, для сообщения игроку). Перезаписывает прежнюю
    // причину. Оповещает subscribeAcquired (гашение GPS). Bounds-safe.
    void acquire(int playerId, std::string reason);
    // Снять лок. Идемпотентно, bounds-safe.
    void release(int playerId);
    bool isLocked(int playerId) const;
    // Причина текущего лока (utf-8) или пустая строка, если лока нет. Bounds-safe.
    const std::string &lockReason(int playerId) const;
    // Полный сброс слота (дисконнект). Идемпотентно, bounds-safe.
    void reset(int playerId);

    // Подписка на ВЗЯТИЕ лока (для гашения GPS бизнесом). Зовётся в конструкторе
    // подписчика — до любого acquire.
    void subscribeAcquired(AcquireObserver observer);

  private:
    struct Lock
    {
        bool locked = false;
        std::string reason; // utf-8; пусто, когда лока нет
    };

    std::array<Lock, MAX_PLAYERS> m_locks;
    std::vector<AcquireObserver> m_acquiredObservers;
};
