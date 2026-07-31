#pragma once

#include "Services/BusinessService/BusinessService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Services/Core/PlayerStateService/PlayerStateService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "Systems/BusinessShop/BusinessShop.h"
#include "player.hpp"
#include <string>
#include <unordered_map>

// АЗС — ТИП бизнеса. Внутри — та же витрина, что у 24/7 (общий BusinessShop);
// снаружи — колонки: сидя за рулём рядом с точкой можно залить бак за деньги.
//
// Как и 24/7, регистрирует себя в BusinessService (имя, пул интерьеров, меню
// посетителя) — общий привод про АЗС не знает ничего.
//
// ЗАПРАВКА РАБОТАЕТ ВСЕГДА, даже у ничейной точки: это инфраструктура мира, без
// неё машины встают намертво. Владение решает лишь, КОМУ идёт выручка — у
// ничейной АЗС деньги просто уходят из экономики (сток), в копилку класть некому.
//
// Топливо — VehicleService (серверная правда о баке); сюда оно приходит только
// через refuel, своего учёта бензина здесь нет.
class GasStationSystem : public BaseSystem
{
  public:
    // Ключ склада ТОПЛИВА у точки. Это НЕ тип предмета: бензин в инвентаре не лежит,
    // но на станции он кончается и владелец его дозаказывает. Значение мимо типов
    // вещей и мимо ключа телефона (1001), иначе склады слиплись бы.
    static constexpr int STOCK_FUEL = 1002;
    // Полный резервуар станции, литров. Это же число видно на табличке у колонки.
    static constexpr int FUEL_STOCK_CAP = 1000;

    GasStationSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // АЗС, в чью КОЛОНКУ попадает точка; -1 — рядом колонки нет. Станции без
    // заданной колонки не обслуживают: их пропускаем.
    int stationAtPump(const Vector3 &position) const;

    // /buyfuel: диалог заправки для водителя, стоящего у колонки.
    void showFuelMenu(IPlayer &player);
    void showLitresInput(IPlayer &player, int businessId);
    // Залить litres литров (кламп по свободному месту в баке и по складу) с оплатой.
    void refuel(IPlayer &player, int businessId, float litres);

    // --- таблички у колонок ---
    // Пересобрать лейблы всех станций: колонку могли задать, убрать или снести сам
    // бизнес. Дёшево — станций десятки, зовётся на изменение контента.
    void rebuildPumpLabels();
    // Обновить текст одной таблички (изменился остаток топлива).
    void refreshPumpLabel(int businessId);
    std::string pumpText(int businessId) const;

    BusinessService &m_businessService;
    PlayerDialogService &m_dialogService;
    PlayerLocationService &m_locationService;
    PlayerStateService &m_stateService;
    PlayerMoneyService &m_moneyService;
    VehicleService &m_vehicleService;
    TextLabelService &m_labelService;
    BusinessShop m_shop;

    // businessId -> id лейбла у его колонки. Нет записи — таблички нет.
    std::unordered_map<int, int> m_pumpLabels;
};
