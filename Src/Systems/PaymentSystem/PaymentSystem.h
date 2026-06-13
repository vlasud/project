#pragma once

#include "Services/Core/PlayerAnimationService/PlayerAnimationService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Передача денег из рук в руки: /pay [id] [сумма]. Перевод проходит, только если
// получатель рядом (серверная дистанция ≤ PAY_RADIUS), в том же виртуальном мире
// и интерьере. Баланс серверно-авторитетный (PlayerMoneyService), деньги
// списываются и зачисляются абсолютным setMoney в одном обработчике (атомарно).
// Плательщик проигрывает прерываемую анимацию передачи. Антифлуд команд —
// корневой, в PlayerCommandService (см. Docs/CommandFlood.md).
class PaymentSystem : public BaseSystem
{
  public:
    PaymentSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    PlayerMoneyService &m_moneyService;
    PlayerLocationService &m_locationService;
    PlayerAnimationService &m_animationService;
};
