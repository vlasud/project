#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include <array>
#include <string>

class PlayerAuthService final : public IService
{
  public:
    bool isPlayerAuthenticated(size_t playerId) const;
    void setPlayerAuthenticated(size_t playerId, bool authenticated);
    void setPlayerPassword(size_t playerId, const std::string &password);
    const std::string &getPlayerPassword(size_t playerId) const;

  private:
    std::array<bool, MAX_PLAYERS> m_authenticatedPlayers{};
    std::array<std::string, MAX_PLAYERS> m_playersPasswords{};
};
