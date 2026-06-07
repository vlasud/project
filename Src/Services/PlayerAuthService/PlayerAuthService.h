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
    void setPlayerPasswordHash(size_t playerId, const std::string &password);
    const std::string &getPlayerPasswordHash(size_t playerId) const;

  private:
    std::array<bool, MAX_PLAYERS> m_authenticatedPlayers{};
    std::array<std::string, MAX_PLAYERS> m_playersPasswordHashs{};
};
