#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include <array>
#include <cstdint>

class MedicJobSystem;

// Персистентный кошелёк заработка врача — источник правды о накопленных, но ещё не
// выданных на руки деньгах ЗА АККАУНТ. Бизнес-фича, НЕ Core.
//
// Каждый вылеченный игрок зачисляется сюда WRITE-THROUGH (в БД пишем в момент
// лечения, не откладываем до конца смены/дисконнекта) — крах сервера теряет максимум
// одно последнее начисление. Выдача на руки — «Забрать деньги» на пикапе больницы,
// доступное в любой момент, не привязанное к смене.
//
// ОТДЕЛЬНАЯ величина от PlayerMoneyService (наличные, сессионные, НЕ персистятся):
// кошелёк персистентен в БД (Sql/schema_all.sql, medic_wallet), как bank_account/
// port_wallet/bus_wallet/hauler_wallet. Наличные выдаются ПОВЕРХ при withdraw().
//
// Дословный близнец BusWalletService/PortWalletService/HaulerWalletService (иная
// таблица) — обобщение в общий «кошелёк работы» отмечено кандидатом в
// Docs/Refactoring.md; четвёртый близнец повышает цену обобщения, но не меняет
// решения: трогать четыре рабочие фичи разом ради дедупликации — отдельная задача.
class MedicWalletService final : public IService
{
    friend MedicJobSystem;

  public:
    using AccountId = PlayerSessionService::AccountId;

    // Баланс кошелька онлайн-игрока (0 — нет слота/пуст). Синхронно из кэша,
    // без обращения к БД (для попапа лечения/«Информации»/гейта на клике).
    std::int64_t balanceOf(int playerId) const;

    // Начислить за вылеченного: кэш += amount, затем write-through UPSERT.
    // amount <= 0 / NO_ACCOUNT / bounds-промах — no-op.
    void add(int playerId, AccountId accountId, std::int64_t amount);

    // Забрать весь баланс: кэш ОБНУЛЯЕТСЯ СРАЗУ (анти-дюп двойного клика/висящего
    // диалога), затем write-through (относительное списание). Возвращает сумму К
    // ВЫДАЧЕ; 0 — нечего забирать/NO_ACCOUNT/bounds-промах.
    std::int64_t withdraw(int playerId, AccountId accountId);

  private:
    // --- вызывается ТОЛЬКО MedicJobSystem (лайфцикл сессии) ---

    // Положить баланс в память на старте сессии (БЕЗ записи в БД — загрузка).
    void load(int playerId, std::int64_t balance);

    // Сброс слота памяти на конце сессии. БД НЕ трогает — баланс остаётся.
    void reset(int playerId);

    std::array<std::int64_t, MAX_PLAYERS> m_balance{}; // value-init -> 0 для всех
};
