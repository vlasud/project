#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include <array>
#include <cstdint>

class PlayerPersonalSkinSystem;

// Источник правды о ЛИЧНОМ (гражданском) скине АККАУНТА на время сессии.
// Персистится в БД (`player.skin`). Это НЕ применённый скин (тот —
// PlayerSkinService): пока игрок состоит во фракции, ОТОБРАЖАЕТСЯ скин
// организации, а его личный скин хранится здесь неизменным и возвращается при
// увольнении. Поэтому на конце сессии в `player.skin` пишется значение ОТСЮДА,
// а не текущий (возможно органный) скин из PlayerSkinService.
//
// Чистый контейнер состояния (как PlayerSkinService): применение к игроку,
// загрузка/запись в БД и привязка к сессии — на PlayerPersonalSkinSystem и
// оркестраторах (auth/faction). Сервисы между собой не связаны (регистр
// конструирует их без зависимостей).
//
// Личный скин держим всегда ВАЛИДНЫМ: setSkin валидирует и игнорирует мусор,
// загрузчик при сидинге даёт фолбэк на дефолт. Невалидное значение из БД не
// доедет ни до применения, ни до возврата при увольнении.
class PlayerPersonalSkinService final : public IService
{
    friend PlayerPersonalSkinSystem;

  public:
    // Все слоты — «не сидированы» до коннекта (getSkin вернёт дефолт): без
    // инициализации чтение неприсоединённого слота было бы мусором.
    PlayerPersonalSkinService();

    // Гражданские дефолты по полу (валидные SA-педы, проходят isValidSkin).
    // Мужской — 78, женский — 77.
    static constexpr int DEFAULT_SKIN_MALE = 78;
    static constexpr int DEFAULT_SKIN_FEMALE = 77;

    // Дефолтный личный скин по полу (0 = муж, иначе жен — как ESex в auth).
    static int defaultSkinForSex(std::uint8_t sex);

    // Записать личный скин в память (источник правды на время сессии). Невалидный
    // id игнорируется (возвращает false) — состояние остаётся валидным.
    // ПРИМЕНЕНИЕ к игроку и запись в БД — НЕ здесь (оркестратор решает, можно ли
    // сейчас показать: член фракции отображается в органном скине).
    bool setSkin(int playerId, int skinId);

    // Личный скин аккаунта (для возврата при увольнении из фракции). Если слот
    // ещё не сидирован (оффлайн / до загрузки) — DEFAULT_SKIN_MALE.
    int getSkin(int playerId) const;

  private:
    // Вызывается PlayerPersonalSkinSystem на коннекте/дисконнекте.
    void resetPlayer(int playerId);

    // -1 — слот не сидирован: getSkin вернёт дефолт.
    std::array<int, MAX_PLAYERS> m_personalSkin;
};
