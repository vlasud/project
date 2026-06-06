#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include <array>

class PlayerAuthService final : public IService
{
  public:
    bool isPlayerAuthenticated(size_t playerId) const;
    void setPlayerAuthenticated(size_t playerId, bool authenticated);

  private:
    std::array<bool, MAX_PLAYERS> m_authenticatedPlayers;
};
