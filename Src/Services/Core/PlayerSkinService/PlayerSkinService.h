#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>
#include <functional>
#include <vector>

class PlayerSkinSystem;

// Единственный источник правды о ПРИМЕНЯЕМОМ скине игрока. Разделяет БАЗУ и
// ВРЕМЕННЫЙ оверрайд:
//
//   m_skins.setSkin(player, 23);       // БАЗА: орг/личный скин, переживает респаун
//   m_skins.setTempSkin(player, 100);  // ВРЕМЕННЫЙ: до ближайшего респауна
//   m_skins.getSkin(playerId);         // серверная правда о БАЗЕ
//
// База — это то, в чём игрок спавнится: PlayerSpawnService строит spawn-инфо из
// getSkin() (= база), поэтому смерть и любой респаун возвращают базовый скин.
// Временный оверрайд НЕ попадает в spawn-инфо и сбрасывается на спавне.
//
// Применяемое к клиенту = (temp активен ? temp : база). setSkin меняет базу, но
// показывает её, ТОЛЬКО если temp не активен (иначе на экране остаётся temp).
// setTempSkin показывает temp немедленно; clearTempSkin возвращает базу.
//
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
    // Все слоты в согласованное состояние ДО первого коннекта: база — валидный
    // дефолт, temp — «нет оверрайда». Без этого 0-инициализация дала бы temp == 0
    // (≠ -1) — несидированный слот ложно «имел бы» временный скин.
    PlayerSkinService();

    // Дефолт ОБЯЗАН быть валидным (isValidSkin): 0 запрещён, иначе несидированный
    // слот «протёк» бы в спавн как CJ. 78 — обычный мужской SA-пед (совпадает с
    // мужским гражданским дефолтом); сервис про пол не знает, поэтому берёт его.
    static constexpr int DEFAULT_SKIN = 78;

    // Задать БАЗУ (орг/личный скин). Применяется к клиенту немедленно, ТОЛЬКО
    // если временный оверрайд не активен (иначе на экране остаётся temp, база
    // покажется после ближайшего респауна). false — невалидный id, скин не тронут.
    bool setSkin(IPlayer &player, int skinId);

    // Временный оверрайд: показать skinId немедленно, не меняя базу. Живёт до
    // ближайшего респауна (clearTempSkin на спавне). false — невалидный id.
    bool setTempSkin(IPlayer &player, int skinId);

    // Снять временный оверрайд и вернуть на клиент БАЗУ. Идемпотентно (если temp
    // не активен — ничего не делает).
    void clearTempSkin(IPlayer &player);

    bool hasTempSkin(int playerId) const;

    // Серверная правда о БАЗЕ (без учёта temp). Это значение читает
    // PlayerSpawnService для spawn-инфо.
    int getSkin(int playerId) const;

    static bool isValidSkin(int skinId);

    // Наблюдатель смены БАЗЫ: зовётся при каждом setSkin (база сменилась). Нужен
    // PlayerSpawnService — спавн-инфо (скин в class-данных) обязано совпадать с
    // базой ВСЕГДА: нативный респаун после смерти берёт скин ИМЕННО из спавн-инфо,
    // поэтому при любой смене базы спавн-инфо надо пересобрать (иначе первая же
    // смерть откатит игрока на прежнюю базу).
    using BaseSkinObserver = std::function<void(IPlayer &)>;
    void subscribeBaseChange(BaseSkinObserver observer);

  private:
    // Вызывается PlayerSkinSystem.
    void resetPlayer(int playerId);

    static bool isValidId(int playerId);

    // База: конструктор и resetPlayer держат валидный DEFAULT_SKIN, иначе
    // несидированный слот «протёк» бы в спавн как CJ (skin 0).
    std::array<int, MAX_PLAYERS> m_baseSkin;

    // -1 — временного оверрайда нет (применяется база). resetPlayer на коннекте
    // и clearTempSkin/спавн возвращают сюда -1; конструктор сидирует тем же.
    std::array<int, MAX_PLAYERS> m_tempSkin;

    // Подписчики на смену базы (PlayerSpawnService) — гоняются в setSkin.
    std::vector<BaseSkinObserver> m_baseObservers;
};
