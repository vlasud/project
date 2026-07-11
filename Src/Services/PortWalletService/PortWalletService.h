#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include <array>
#include <cstdint>

class PortJobSystem;

// Персистентный кошелёк заработка в порту — источник правды о накопленных, но
// ещё не выданных на руки деньгах ЗА АККАУНТ. Бизнес-фича, НЕ Core.
//
// Каждая сданная в порту коробка зачисляется сюда WRITE-THROUGH (в БД пишем в
// момент сдачи, не откладываем до конца смены/дисконнекта) — крах сервера
// теряет максимум одну коробку, которая несётся прямо сейчас (её и так роняет
// PortJobService::dropCarry при смерти). Выдача на руки — отдельное действие
// «Забрать деньги» на пикапе порта, доступное в любой момент, не привязанное
// к смене.
//
// ОТДЕЛЬНАЯ величина от PlayerMoneyService (наличные, сессионные, НЕ
// персистятся): кошелёк порта персистентен в БД (Sql/schema_all.sql,
// port_wallet), как bank_account. Наличные выдаются ПОВЕРХ при withdraw().
//
// Персист — write-through, как членство фракций/выбор спавна: память меняется
// синхронно, БД пишется тем же вызовом (UPSERT). На старте сессии PortJobSystem
// грузит баланс по account_id (serial-guard) и кладёт в память через load();
// reset() на конце сессии ЧИСТИТ ТОЛЬКО ПАМЯТЬ (баланс остаётся в БД).
class PortWalletService final : public IService
{
    friend PortJobSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Баланс кошелька онлайн-игрока (0 — нет слота/пуст). Синхронно из кэша,
    // без обращения к БД (для попапа сдачи/«Информации»/гейта на клике).
    std::int64_t balanceOf(int playerId) const;

    // Начислить за сданную коробку: кэш += amount, затем write-through UPSERT
    // (INSERT ... ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance),
    // как BankService::deposit). amount <= 0 / NO_ACCOUNT / bounds-промах —
    // no-op (без accountId копить некуда, отрицательного баланса быть не может).
    void add(int playerId, AccountId accountId, std::int64_t amount);

    // Забрать весь баланс: кэш ОБНУЛЯЕТСЯ СРАЗУ (анти-дюп двойного клика/
    // висящего диалога), затем write-through (относительное списание). Возвращает сумму К
    // ВЫДАЧЕ (то, что было в кэше до обнуления); 0 — нечего забирать/NO_ACCOUNT/
    // bounds-промах — вызывающий не платит наличные.
    std::int64_t withdraw(int playerId, AccountId accountId);

  private:
    // --- вызывается ТОЛЬКО PortJobSystem (лайфцикл сессии) ---

    // Положить баланс в память на старте сессии (БЕЗ записи в БД — загрузка).
    void load(int playerId, std::int64_t balance);

    // Сброс слота памяти на конце сессии. БД НЕ трогает — баланс остаётся,
    // следующий логин того же аккаунта загрузит его заново через load().
    void reset(int playerId);

    std::array<std::int64_t, MAX_PLAYERS> m_balance{}; // value-init -> 0 для всех
};
