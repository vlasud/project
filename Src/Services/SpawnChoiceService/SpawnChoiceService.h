#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "player.hpp"
#include <array>
#include <cstdint>

class SpawnChoiceSystem;

// Источник правды о ВЫБОРЕ точки спавна игрока (/setspawn) — где он появляется
// при входе в игру и после смерти. Бизнес-фича, НЕ Core.
//
// Выбор главнее фракции: игрок ВСЕГДА может выбрать точку спавна, независимо от
// членства в организации. Пункты — вокзал/дом/работа; Work — база фракции
// (доступна только члену орга с заданной точкой спавна). Дефолт для всех, кто не
// выбирал, — ЖД вокзал.
//
// Хранит лишь НАМЕРЕНИЕ (вокзал/дом/работа), а не координаты: конкретную точку
// резолвит SpawnChoiceSystem от серверных фактов в момент применения (дом из
// HouseService, база — из FactionService; иначе фолбэк на вокзал). Поэтому
// продажа дома / выход из орга не оставляют битой точки — резолв на следующем
// применении даёт валидную.
//
// Выбор персистится per-аккаунт в БД (Sql/player_spawn.sql) write-through, как
// членство фракций: память -> сразу в БД (REPLACE в транзакции). На старте
// сессии SpawnChoiceSystem грузит выбор по account_id (serial-guard) и кладёт в
// память через load(); reset() на конце сессии. Дефолт (нет записи в БД) — вокзал.
class SpawnChoiceService final : public IService
{
    friend SpawnChoiceSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Индексы совпадают с колонкой choice в БД — порядок менять нельзя.
    // Неизвестное значение нормализуется в Station при загрузке.
    enum class Choice : int
    {
        Station = 0, // ЖД вокзал (дефолт и фолбэк)
        Home = 1,    // вход дома, которым владеет аккаунт
        Work = 2,    // база фракции (только член орга с заданной точкой спавна)
    };

    // Текущий выбор игрока онлайн (Station — нет слота/невалидный playerId).
    Choice getChoice(int playerId) const;

    // Сохранить выбор: память + write-through в БД (REPLACE player_spawn в
    // транзакции). Требует активной сессии (accountId из неё, не от клиента);
    // невалидный playerId/NO_ACCOUNT — no-op. Сам выбор уже провалидирован
    // вызывающим (Home — владение домом, Work — членство + точка спавна орга).
    void setChoice(IPlayer &player, AccountId accountId, Choice choice);

  private:
    // --- вызывается SpawnChoiceSystem ---
    // Положить выбор в память на старте сессии (БЕЗ записи в БД — это загрузка).
    void load(int playerId, Choice choice);
    void reset(int playerId);

    std::array<Choice, MAX_PLAYERS> m_choice{}; // value-init -> Station(0) для всех
};
