#pragma once

#include "Services/Core/TimerService/TimerService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "types.hpp"

// Периодический автосейв ОНЛАЙН-игроков: раз в AUTOSAVE_INTERVAL прогоняет
// save-канал PlayerSessionService по всем активным сессиям (идемпотентные
// персисты: вещи, владение оружием, личный скин).
//
// Зачем: open.mp на штатной остановке НЕ диспатчит onPlayerDisconnect (см.
// исходники сервера), поэтому session-end-персист на shutdown для остававшихся
// онлайн-игроков не срабатывает — их изменения за сессию терялись бы. Автосейв
// ограничивает потерю интервалом (покрывает и краш). Zero-loss потребовал бы
// хрупкого shutdown-flush — намеренно НЕ делаем (см. Docs/Autosave.md).
//
// Деструктивные подписчики (Faction/Admin teardown) сидят на subscribeEnd, НЕ в
// save-канале — автосейв их не трогает, иначе периодически обнулял бы членство и
// админку у онлайн-игроков.
class AutosaveSystem : public BaseSystem
{
  public:
    AutosaveSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // Тюнинг: интервал автосейва. Меньше — меньше потеря при краше/штатном стопе,
    // выше всплеск БД-записей при большом онлайне.
    static constexpr Minutes AUTOSAVE_INTERVAL{2};

    // Прогнать save() по всем активным сессиям онлайн-игроков.
    void saveAll();

    PlayerSessionService &m_sessionService;
    TimerService &m_timerService;
    TimerService::Handle m_timer;
};
