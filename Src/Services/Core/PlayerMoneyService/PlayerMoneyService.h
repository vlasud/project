#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>

// Сервис денег — серверно-авторитетный баланс игрока.
//
// Сервис ведёт собственный баланс как единственную правду: setMoney/giveMoney
// КОНТРАКТ: любое изменение денег — ТОЛЬКО через сервис. Прямой player.giveMoney()
class PlayerMoneyService final : public IService
{
  public:
    // --- источник правды ---
    unsigned long long getMoney(int playerId) const; // серверный баланс (НЕ клиентский getMoney())

    // --- серверные операции ---
    void setMoney(IPlayer &player, unsigned long long amount);  // абсолютная установка
    void giveMoney(IPlayer &player, unsigned long long amount); // прибавить (может быть отрицательным)

    void reset(int playerId);

  private:
    std::array<unsigned long long, MAX_PLAYERS> m_state;
};
