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
    // Дефолт ОБЯЗАН быть валидным (isValidSkin): 0 запрещён, иначе несидированный
    // слот «протёк» бы в спавн как CJ. 78 — обычный мужской SA-пед (совпадает с
    // мужским гражданским дефолтом); сервис про пол не знает, поэтому берёт его.
    static constexpr int DEFAULT_SKIN = 78;

    // false — невалидный id (вне 1..311 или несуществующий 74), скин не тронут.
    bool setSkin(IPlayer &player, int skinId);
    int getSkin(int playerId) const;

    static bool isValidSkin(int skinId);

  private:
    // Вызывается PlayerSkinSystem.
    void resetPlayer(int playerId);

    // 0-инициализация массива — это «слот не сидирован»; getSkin его не
    // фильтрует, поэтому resetPlayer обязан проставить валидный DEFAULT_SKIN до
    // первого использования (делается на onConnect). См. resetPlayer.
    std::array<int, MAX_PLAYERS> m_skins{};
};
