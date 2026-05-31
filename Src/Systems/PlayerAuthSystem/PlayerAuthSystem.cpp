#include "PlayerAuthSystem.h"
#include "Strings.h"

#include "../../Pools/PlayerPool.h"
#include "core.hpp"

#include <fmt/core.h>

void PlayerAuthSystem::link(ICore *core)
{
    core->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
}

void PlayerAuthSystem::initialize()
{
}

void PlayerAuthSystem::reset()
{
}

void PlayerAuthSystem::onPlayerConnect(IPlayer &player)
{
    Player *newPlayer = PlayerPool::get(player.getID());
    if (!newPlayer)
    {
        player.sendClientMessage(Colour::White(), PLAYER_AUTH_SYSTEM_INVALID_ID);
        player.kick();
        return;
    }

    if (newPlayer->getIsLoggedIn())
    {
        player.sendClientMessage(Colour::White(), PLAYER_AUTH_SYSTEM_ALREADY_LOGGED_IN);
        player.kick();
        return;
    }
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    Player *newPlayer = PlayerPool::get(player.getID());
    if (!newPlayer)
    {
        return;
    }

    newPlayer->setIsLoggedIn(false);
}
