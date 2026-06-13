#pragma once

#include "Macro.h"
#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include "types.hpp"
#include <array>

// Передача денег из рук в руки: /pay [id] [сумма]. Перевод проходит, только если
// получатель рядом (серверная дистанция ≤ PAY_RADIUS), в том же виртуальном мире
// и интерьере. Баланс серверно-авторитетный (PlayerMoneyService), деньги
// списываются и зачисляются абсолютным setMoney в одном обработчике (атомарно).
// Плательщик проигрывает прерываемую анимацию передачи.
class PaymentSystem : public BaseSystem
{
  public:
    PaymentSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    PlayerMoneyService &m_moneyService;
    PlayerLocationService &m_locationService;
    PlayerAnimationService &m_animationService;

    // Время последнего успешного перевода на игрока — антифлуд (см. PAY_COOLDOWN).
    std::array<TimePoint, MAX_PLAYERS> m_lastPay{};
};
