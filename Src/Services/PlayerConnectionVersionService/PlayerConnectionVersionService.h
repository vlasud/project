#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include <array>

class PlayerConnectionVersionService final : public IService
{
  public:
    void changeVersion(int playerID);
    int getVersion(int playerID) const;

  private:
    std::array<int, MAX_PLAYERS> m_playerConnectionVersions{};
};
