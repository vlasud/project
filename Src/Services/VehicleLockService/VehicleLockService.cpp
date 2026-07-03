#include "Services/VehicleLockService/VehicleLockService.h"

#include <core.hpp>

void VehicleLockService::bind(VehicleService &vehicleService, PersonalVehicleService &personalService,
                              ParkedVehicleService &parkedService, FamilyService &familyService,
                              PlayerSessionService &sessionService, ICore &core)
{
    m_vehicleService = &vehicleService;
    m_personalService = &personalService;
    m_parkedService = &parkedService;
    m_familyService = &familyService;
    m_sessionService = &sessionService;
    m_core = &core;
}

bool VehicleLockService::isLocked(int vehicleId) const
{
    const auto it = m_locked.find(vehicleId);
    return it != m_locked.end() && it->second;
}

bool VehicleLockService::allowedWhenLocked(int vehicleId, AccountId accountId) const
{
    if (accountId == PlayerSessionService::NO_ACCOUNT)
    {
        return false; // не залогинен — пускать некого
    }

    // Припаркованная (у дома/в семье) — источник правды записи ParkedVehicleService:
    // владелец МАШИНЫ по accountId, либо член семьи, если расшарена.
    if (m_parkedService)
    {
        if (const ParkedVehicleService::Parked *parked = m_parkedService->byVehicleId(vehicleId))
        {
            if (parked->ownerAccountId == accountId)
            {
                return true;
            }
            if (parked->familyId != FamilyService::NO_FAMILY && m_familyService)
            {
                return m_familyService->familyByAccount(accountId) == parked->familyId;
            }
            return false;
        }
    }

    // Не припаркована — обычная личная сессионная (Owner::Player): владелец —
    // тот же аккаунт, что держит запись PersonalVehicleService по этому vehicleId.
    // Ищем по всем игрокам (мало, холодный путь) — как onWorldVehicleDestroyed.
    if (m_personalService && m_vehicleService && m_sessionService && m_core)
    {
        if (m_vehicleService->getOwner(vehicleId) == VehicleService::Owner::Player)
        {
            const int ownerId = m_vehicleService->getOwnerId(vehicleId);
            if (ownerId < 0)
            {
                return false;
            }
            const AccountId ownerAccountId = m_sessionService->getAccountId(ownerId);
            return ownerAccountId != PlayerSessionService::NO_ACCOUNT && ownerAccountId == accountId;
        }
    }
    return false;
}

void VehicleLockService::applyToPlayer(IVehicle &vehicle, IPlayer &player)
{
    if (!m_vehicleService || !m_sessionService)
    {
        return;
    }
    const int vehicleId = vehicle.getID();
    const bool locked = isLocked(vehicleId);
    if (!locked)
    {
        m_vehicleService->setLockedForPlayer(vehicle, player, false);
        return;
    }
    const PlayerSessionService::AccountId accountId = m_sessionService->getAccountId(player.getID());
    const bool allowed = allowedWhenLocked(vehicleId, accountId);
    m_vehicleService->setLockedForPlayer(vehicle, player, !allowed);
}

void VehicleLockService::reapplyToStreamed(int vehicleId)
{
    if (!m_vehicleService || !m_core)
    {
        return;
    }
    IVehicle *vehicle = m_vehicleService->get(vehicleId);
    if (!vehicle)
    {
        return;
    }
    // O(MAX_PLAYERS) — только на редких событиях (флип замка, share/unshare),
    // не per-tick. isStreamedInForPlayer — обёртка VehicleService (сырой SDK не
    // трогаем из бизнеса).
    for (IPlayer *player : m_core->getPlayers().entries())
    {
        if (m_vehicleService->isStreamedInForPlayer(*vehicle, *player))
        {
            applyToPlayer(*vehicle, *player);
        }
    }
}

void VehicleLockService::setLocked(int vehicleId, bool locked)
{
    if (!m_vehicleService || !m_vehicleService->get(vehicleId))
    {
        return; // несуществующая машина — bounds/exists-safe
    }
    m_locked[vehicleId] = locked;
    reapplyToStreamed(vehicleId);
}

bool VehicleLockService::toggle(int vehicleId)
{
    setLocked(vehicleId, !isLocked(vehicleId));
    // Читаем ФАКТИЧЕСКОЕ состояние после вызова: setLocked — no-op для
    // несуществующей машины, и isLocked тут покажет, что запрос не применился.
    return isLocked(vehicleId);
}

void VehicleLockService::onStreamedInForPlayer(IVehicle &vehicle, IPlayer &player)
{
    // Ядро НЕ восстанавливает пер-игровые params на стрим-ине — переприменяем
    // ТОЛЬКО если у машины вообще есть решение (запись в m_locked); иначе (машина
    // никогда не запиралась) не шлём лишний RPC — дефолт клиента и так «открыто».
    if (m_locked.find(vehicle.getID()) == m_locked.end())
    {
        return;
    }
    applyToPlayer(vehicle, player);
}

void VehicleLockService::onDestroyed(int vehicleId)
{
    m_locked.erase(vehicleId); // новая жизнь машины (респавн НЕ уничтожает — id тот же) замок не наследует только на реальном destroy
}

void VehicleLockService::onReconcile(long long dbId)
{
    // share/unshare семье меняет состав «свой» для уже закрытой машины — если она
    // сейчас заперта, переприменяем ко всем застримленным немедленно (новый член
    // семьи должен тут же начать проходить, снятый с шеринга — перестать).
    if (!m_parkedService)
    {
        return;
    }
    const ParkedVehicleService::Parked *parked = m_parkedService->byDbId(dbId);
    if (!parked || parked->vehicleId == -1)
    {
        return;
    }
    if (isLocked(parked->vehicleId))
    {
        reapplyToStreamed(parked->vehicleId);
    }
}
