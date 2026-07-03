#pragma once

#include "Macro.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/IService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "player.hpp"
#include <unordered_map>

struct ICore;
class VehicleLockSystem;

// Замок дверей личного транспорта — бизнес-фича (НЕ Core). Источник правды о том,
// какие живые машины (по vehicleId) закрыты игроком-владельцем ПРЯМО СЕЙЧАС.
// Состояние ЧИСТО СЕССИОННОЕ, НЕ персистится — замок сбрасывается при уничтожении
// экземпляра (новая жизнь машины открыта по умолчанию, как всегда было).
//
// ПОЧЕМУ ПЕР-ИГРОКОВЫЙ, А НЕ ГЛОБАЛЬНЫЙ (VehicleService::setLocked):
// глобальный клиентский замок блокирует ВХОД ВСЕМ, включая владельца. Владелец,
// закрывший свою машину и вышедший из неё, оказался бы заперт СНАРУЖИ навсегда
// (открыть можно только сидя внутри — гейт /car требует «в своей машине»,
// открыть/закрыть может только владелец). Пер-игровой params.doors
// (VehicleService::setLockedForPlayer) решает это: замок применяется всем, КРОМЕ
// владельца (и членов семьи, если машина расшарена) — их VehicleParams для
// конкретного игрока просто не переставляются в «заперто».
//
// ПОЛИТИКА ДОСТУПА при закрытой машине: НЕ заперта для владельца (accountId
// владельца записи — сессионной личной или припаркованной) и для членов семьи,
// если машина расшарена (parkedMode == familyId, FamilyService — членство).
// Для всех прочих (в т.ч. посторонних игроков без сессии) — заперта.
//
// ПРИМЕНЕНИЕ: флип замка (setLocked) проходит по подключённым игрокам, которым
// машина застримлена (VehicleService::isStreamedInForPlayer), и ставит каждому
// его персональный doors по политике; на стрим-ине машины НОВОМУ игроку (ядро САМ
// пер-игровые params не восстанавливает — см. VehicleService::
// subscribeStreamedInForPlayer) применяется то же самое для этого игрока; на
// share/unshare семье (ParkedVehicleService::subscribeReconcile стреляет на флип
// family_id) состав «свой» меняется — переприменяется всем застримленным; на
// уничтожении экземпляра (VehicleService::subscribeDestroyed) запись забывается.
//
// Событийное (клик /car, стрим-ин, share/unshare, destroy) — per-tick работы нет;
// применение к застримленным — O(MAX_PLAYERS) на редких событиях (флип/share).
class VehicleLockService final : public IService
{
    friend VehicleLockSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Привязать зависимости (реестр создаёт сервис дефолтным ctor; bind — в
    // конструкторе VehicleLockSystem, как ParkedVehicleService::bind).
    void bind(VehicleService &vehicleService, PersonalVehicleService &personalService,
              ParkedVehicleService &parkedService, FamilyService &familyService,
              PlayerSessionService &sessionService, ICore &core);

    // Заперта ли машина сейчас (сессионный флаг; false для незакрытой/неизвестной).
    bool isLocked(int vehicleId) const;

    // Установить замок машины и НЕМЕДЛЕННО переприменить его ко всем застримленным
    // игрокам (по актуальной политике доступа). No-op для несуществующей машины
    // (bounds/exists-safe — CarMenuSystem уже гейтит владельца, это подстраховка).
    void setLocked(int vehicleId, bool locked);
    // Тоггл: setLocked(vehicleId, !isLocked(vehicleId)). Возвращает новое состояние
    // ПОСЛЕ вызова (для несуществующей машины setLocked — no-op, isLocked после него
    // покажет фактический результат, а не «желаемый» — вызывающий не спутает успех
    // с no-op).
    bool toggle(int vehicleId);

  private:
    // Пускает ли текущая политика конкретного игрока БЕЗ замка (владелец записи
    // ЛИБО член семьи, если машина расшарена). accountId — серверный (сессия).
    bool allowedWhenLocked(int vehicleId, AccountId accountId) const;

    // Переприменить текущее состояние замка машины ко ВСЕМ игрокам, которым она
    // застримлена сейчас (O(MAX_PLAYERS), только на редких событиях).
    void reapplyToStreamed(int vehicleId);
    // Применить состояние замка ОДНОМУ игроку (по политике доступа).
    void applyToPlayer(IVehicle &vehicle, IPlayer &player);

    // --- вызывается VehicleLockSystem ---
    void onStreamedInForPlayer(IVehicle &vehicle, IPlayer &player);
    void onDestroyed(int vehicleId);
    void onReconcile(long long dbId); // share/unshare семье — состав «свой» изменился

    VehicleService *m_vehicleService = nullptr;
    PersonalVehicleService *m_personalService = nullptr;
    ParkedVehicleService *m_parkedService = nullptr;
    FamilyService *m_familyService = nullptr;
    PlayerSessionService *m_sessionService = nullptr;
    ICore *m_core = nullptr;

    // vehicleId -> заперта. Отсутствие записи == не заперта (по умолчанию открыта).
    std::unordered_map<int, bool> m_locked;
};
