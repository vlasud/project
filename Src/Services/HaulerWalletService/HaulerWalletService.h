#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include <array>
#include <cstdint>

class HaulerJobSystem;

// Персистентный кошелёк заработка портового развозчика — источник правды о
// накопленных, но ещё не выданных на руки деньгах ЗА АККАУНТ. Бизнес-фича, НЕ Core.
//
// Каждая разгруженная на базе коробка (и бонус за полную разгрузку 10/10)
// зачисляется сюда WRITE-THROUGH (в БД пишем в момент сдачи, не откладываем до
// конца смены/дисконнекта) — крах сервера теряет максимум одно последнее
// начисление. Выдача на руки — отдельное действие «Забрать деньги» на пикапе
// трудоустройства развозчика, доступное в любой момент, не привязанное к смене.
//
// ОТДЕЛЬНАЯ величина от PlayerMoneyService (наличные, сессионные, НЕ персистятся):
// кошелёк персистентен в БД (Sql/schema_all.sql, hauler_wallet), как bank_account/
// port_wallet/bus_wallet. Наличные выдаются ПОВЕРХ при withdraw().
//
// Персист — write-through, как bus_wallet/port_wallet: память меняется синхронно,
// БД пишется тем же вызовом (UPSERT). На старте сессии HaulerJobSystem грузит
// баланс по account_id (serial-guard) и кладёт в память через load(); reset() на
// конце сессии ЧИСТИТ ТОЛЬКО ПАМЯТЬ (баланс остаётся в БД).
//
// Дословный близнец BusWalletService/PortWalletService (иная таблица) — обобщение
// в общий «кошелёк работы» отмечено кандидатом в Docs/Refactoring.md; сейчас НЕ
// обобщаем (три работы — ещё ранняя стадия, риск задеть рабочие фичи).
class HaulerWalletService final : public IService
{
    friend HaulerJobSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Баланс кошелька онлайн-игрока (0 — нет слота/пуст). Синхронно из кэша,
    // без обращения к БД (для попапа сдачи/«Информации»/гейта на клике).
    std::int64_t balanceOf(int playerId) const;

    // Начислить за сданную коробку/бонус: кэш += amount, затем write-through
    // UPSERT (INSERT ... ON DUPLICATE KEY UPDATE balance = balance +
    // VALUES(balance)). amount <= 0 / NO_ACCOUNT / bounds-промах — no-op.
    void add(int playerId, AccountId accountId, std::int64_t amount);

    // Забрать весь баланс: кэш ОБНУЛЯЕТСЯ СРАЗУ (анти-дюп двойного клика/висящего
    // диалога), затем write-through (относительное списание). Возвращает сумму К
    // ВЫДАЧЕ (то, что было в кэше до обнуления); 0 — нечего забирать/NO_ACCOUNT/
    // bounds-промах — вызывающий не платит наличные.
    std::int64_t withdraw(int playerId, AccountId accountId);

  private:
    // --- вызывается ТОЛЬКО HaulerJobSystem (лайфцикл сессии) ---

    // Положить баланс в память на старте сессии (БЕЗ записи в БД — загрузка).
    void load(int playerId, std::int64_t balance);

    // Сброс слота памяти на конце сессии. БД НЕ трогает — баланс остаётся,
    // следующий логин того же аккаунта загрузит его заново через load().
    void reset(int playerId);

    std::array<std::int64_t, MAX_PLAYERS> m_balance{}; // value-init -> 0 для всех
};
