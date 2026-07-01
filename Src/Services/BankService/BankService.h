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
//   m_bank.creditFactionSalaries(factionId, budgetCap, cb); // зарплаты атомарно
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

    // Зарплатные чеки всем членам фракции (и оффлайн) ОДНОЙ транзакцией:
    // суммирует зарплаты и зачисляет каждому его персональную зарплату из ОДНОГО
    // снимка faction_member.salary — сумма списания (callback) равна сумме
    // зачислений по построению (TOCTOU «печати денег» исключён: правка зарплаты
    // лидером между чтениями невозможна, чтение и кредит в одной транзакции).
    // budgetCap — потолок: если суммарная зарплата его превышает, НЕ зачисляем
    // ничего. callback (главный поток) получает реально зачисленную сумму:
    //   > 0 — зачислено столько, спиши ровно это с бюджета;
    //   = 0 — платить некому ИЛИ не хватило бюджета (бюджет не трогать).
    // Различить эти два случая вызывающий может сравнением budgetCap с заранее
    // показанной суммой; для приказа достаточно «0 — ничего не списано».
    // errorCallback (главный поток) вызывается при сбое транзакции (rollback):
    // деньги не зачислены, бюджет не тронут — вызывающий откатывает кулдаун
    // приказа и сообщает лидеру «повторите».
    void creditFactionSalaries(int factionId, std::int64_t budgetCap,
                               std::function<void(std::int64_t creditedTotal)> callback,
                               std::function<void()> errorCallback);

    // Баланс счёта; 0 — счёта ещё нет. Колбэк на главном потоке.
    void getBalance(AccountId accountId, std::function<void(std::int64_t)> callback);
};
