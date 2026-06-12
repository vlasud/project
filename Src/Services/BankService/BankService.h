#pragma once

#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include <cstdint>
#include <functional>

// Банковский счёт аккаунта — фундамент банковской системы. Деньги организаций
// и зарплатные чеки живут здесь; наличные игрока — PlayerMoneyService.
//
// Источник правды — строка bank_account в БД (сервер — единственный
// писатель), в памяти ничего не кэшируется: счёт меняется и для ОФФЛАЙН
// аккаунтов (зарплатные чеки по приказу), кэш бы расходился. Все операции
// асинхронные:
//
//   m_bank.deposit(accountId, 500);                       // зачислить (чек)
//   m_bank.creditFactionSalaries(factionId);              // чеки всем членам фракции
//   m_bank.getBalance(accountId, [](std::int64_t b) {...}); // колбэк на главном потоке
//
// Города-банки как фракции (кассы, ограбления) — будущий геймплей поверх
// этого фундамента; снятие/перевод появятся вместе с ним.
class BankService final : public IService
{
  public:
    using AccountId = PlayerSessionService::AccountId;

    // Зачислить на счёт (счёт создаётся при первом зачислении).
    void deposit(AccountId accountId, std::int64_t amount);

    // Зарплатные чеки всем членам фракции (и оффлайн) одним запросом:
    // каждому — его персональная зарплата из faction_member.
    void creditFactionSalaries(int factionId);

    // Баланс счёта; 0 — счёта ещё нет. Колбэк на главном потоке.
    void getBalance(AccountId accountId, std::function<void(std::int64_t)> callback);
};
