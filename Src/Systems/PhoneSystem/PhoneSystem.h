#pragma once

#include "Macro.h"
#include "Services/Core/AudioService/AudioService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerHealthService/PlayerHealthService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/PhoneService/PhoneService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/VehicleWaypointService/VehicleWaypointService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Телефон — привод PhoneService. Бизнес-фича, НЕ Core.
//
//  /c              — меню: скорая / полиция / такси / набрать номер;
//  /c <номер>      — быстрый набор номера (пункт 4 без меню);
//  /acceptjob <N>  — работник принимает вызов N.
//
// Вызов службы рассылается ТОЛЬКО тем, кто у неё сейчас на смене: список приносит
// сама работа (саморегистрация в PhoneService), телефон про работы не знает. Каждому
// работнику дистанция считается СВОЯ — от него до звонящего.
//
// Принятие забирает вызов у остальных (первый успел — заказ исчез), ставит принявшему
// указатель на место звонка и сообщает звонящему, что помощь выехала.
//
// Номер телефона выдаётся аккаунту один раз при первом входе и живёт в player.phone;
// загрузка/выдача идут одним async-запросом на старте сессии (serial-guard).
class PhoneSystem : public BaseSystem
{
  public:
    PhoneSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // --- команды ---
    void onCallCommand(IPlayer &player, const std::string &argument);
    void showCallMenu(IPlayer &player);
    void showDialNumberDialog(IPlayer &player);
    void onAcceptJobCommand(IPlayer &worker, int orderNumber);

    // --- вызовы служб ---
    void placeServiceCall(IPlayer &player, PhoneService::Service service);
    void dialNumber(IPlayer &player, std::int64_t phone);

    // --- лайфцикл сессии ---
    void loadPhone(IPlayer &player, const PlayerSessionService::Session &session);
    void onSessionEnd(IPlayer &player);

    PhoneService &m_phoneService;
    PlayerSessionService &m_sessionService;
    PlayerLocationService &m_locationService;
    PlayerHealthService &m_healthService;
    FactionService &m_factionService;
    VehicleWaypointService &m_waypointService;
    PlayerDialogService &m_dialogService;
    AudioService &m_audioService;
};
