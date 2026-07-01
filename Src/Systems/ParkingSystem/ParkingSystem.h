#pragma once

#include "Macro.h"
#include "Services/Core/CheckpointService/CheckpointService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PersonalVehicleService/PersonalVehicleService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Парковка личного транспорта — бизнес-фича (НЕ Core), способ СПАВНА личной машины
// (владение регистрирует /pvbuy через PersonalVehicleService). Геймплей событийный
// (пикап/диалог/чекпоинт), per-tick работы нет:
//  * пикап парковки (фикс. серверная точка) -> диалог со списком ЛИЧНЫХ машин
//    игрока -> выбор -> машина спавнится на первой СВОБОДНОЙ из 5 серверных точек
//    парковки + красный чекпоинт у неё (ведёт к машине на 5-местной парковке);
//  * «занято» = любая машина физически в радиусе ЛЮБОЙ из 3 проб места (центр +
//    вперёд + назад по ориентации места — покрыть длину прямоугольника,
//    anyVehicleNear); при пере-спавне СВОЯ машина исключается во всех пробах (можно
//    встать на её же точку); все точки заняты — отказ (существующие машины не трогаем);
//  * взрыв/уничтожение машины -> владение остаётся, её спавнят заново через пикап.
//
// Все точки спавна и точка пикапа — СЕРВЕРНЫЕ (фикс.); произвольную позицию игрок
// задать не может. Игрок спавнит ТОЛЬКО свои машины (владение серверное по playerId,
// индекс валидируется в PersonalVehicleService).
class ParkingSystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    ParkingSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    // Точка спавна машины на парковке (серверная, фикс.).
    struct SpawnSpot
    {
        Vector3 position;
        float angle;
    };

    // Per-player состояние красного чекпоинта у поданной машины.
    struct CheckpointState
    {
        bool active = false;
        int vehicleId = -1; // id машины, к которой ведёт чекпоинт (для снятия на её уничтожении)
    };

    // Игрок встал на пикап парковки: нет машин -> сообщение; иначе диалог выбора.
    void onParkingPickup(IPlayer &player);
    // Заспавнить владение ownedIndex игрока на первой свободной точке + чекпоинт.
    void spawnAtParking(IPlayer &player, int ownedIndex);
    // Место свободно, только если НИ ОДНА из 3 проб (центр + вперёд + назад по
    // ориентации места) не нашла машину. excludeVehicleId (своя при пере-спавне)
    // исключается во ВСЕХ пробах. Прямоугольник места длиннее радиуса, поэтому одна
    // круговая проба от центра могла бы упустить машину у переднего/заднего края.
    bool isSpotFree(const SpawnSpot &spot, int excludeVehicleId) const;
    // Снять красный чекпоинт игрока (если активен). Bounds-safe.
    void clearCheckpoint(int playerId);

    PersonalVehicleService &m_personalService;
    VehicleService &m_vehicleService;
    PickupService &m_pickupService;
    CheckpointService &m_checkpointService;
    PlayerDialogService &m_dialogService;
    TextLabelService &m_labelService;

    int m_parkingPickup = -1; // хэндл пикапа парковки
    int m_parkingLabel = -1;  // хэндл 3D-текста над пикапом
    std::array<CheckpointState, MAX_PLAYERS> m_checkpoints{};
};
