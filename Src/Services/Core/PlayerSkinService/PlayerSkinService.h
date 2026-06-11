#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>

class PlayerSkinSystem;

// Единственный источник правды о скине игрока.
//
//   m_skins.setSkin(player, 23);   // применяется сразу и переживает респауны
//   m_skins.getSkin(playerId);     // серверная правда
//
// PlayerSpawnService строит spawn-инфо из этого значения, поэтому смерть и
// любой респаун сохраняют скин — магазин одежды задаёт его одним вызовом.
// Никто не зовёт player.setSkin() напрямую — иначе источников правды станет два
// (и респаун молча откатит скин).
//
// Скин — данные сервер -> клиент: своего скина клиент серверу не сообщает,
// локальный спуф виден только самому читеру. Известный клиентский баг: смена
// скина сидящему в машине может глючить визуально — по возможности меняйте
// скин пешему игроку.
class PlayerSkinService final : public IService
{
    friend PlayerSkinSystem;

  public:
    static constexpr int DEFAULT_SKIN = 0; // CJ

    // false — невалидный id (вне 0..311 или несуществующий 74), скин не тронут.
    bool setSkin(IPlayer &player, int skinId);
    int getSkin(int playerId) const;

    static bool isValidSkin(int skinId);

  private:
    // Вызывается PlayerSkinSystem.
    void resetPlayer(int playerId);

    std::array<int, MAX_PLAYERS> m_skins{}; // 0 — дефолт
};
