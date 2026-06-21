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
    void giveMoney(IPlayer &player, unsigned long long amount); // прибавить к балансу

    // Заново отправить серверный баланс на клиент, НЕ меняя его. Нужно на спавне:
    // GTA-клиент при смерти сам списывает $100 (госпиталь single-player GTA:SA), и
    // без ре-синхрона HUD остаётся ниже серверной правды до следующей операции.
    void syncToClient(IPlayer &player) const;

    void reset(int playerId);

  private:
    std::array<unsigned long long, MAX_PLAYERS> m_state;
};
