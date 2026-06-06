#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include <array>

class PlayerConnectionVersionService final : public IService
{
  public:
    void changePlayerConnectionVersion(int playerID);
    int getPlayerConnectionVersion(int playerID) const;

  private:
    std::array<int, MAX_PLAYERS> m_playerConnectionVersions{};
};
