#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include <array>

class PlayerAuthSystem;

class PlayerAuthService final : public IService
{
    friend PlayerAuthSystem;

  public:
    enum class EAuthState : uint8_t
    {
        UNKNOWN = 0,
        AUTHORIZING,
        AUTHENTICATED
    };

    EAuthState getAuthState(size_t playerId) const;

  private:
    void setPlayerAuthenticated(size_t playerId, EAuthState state);

    std::array<EAuthState, MAX_PLAYERS> m_authState{};
};
