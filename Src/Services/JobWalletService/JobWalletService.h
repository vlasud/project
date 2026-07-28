#pragma once

#include "Services/IService.h"
#include "player.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Кошельки работ в одном месте — бизнес-сервис (НЕ Core). Отвечает ровно на один
// вопрос: сколько заработанного лежит у игрока по каждой работе.
//
// Заработок каждой работы копится в её СОБСТВЕННОМ персистентном кошельке
// (BusWalletService/PortWalletService/HaulerWalletService — каждый со своей таблицей),
// а забирают его НА МЕСТЕ, у пикапа этой работы. Игрок, заработавший вчера, про деньги
// попросту не вспоминал. Этот сервис даёт единую СПРАВКУ (/jobwallet, привод —
// JobWalletSystem) и напоминание на входе, когда забрать действительно есть что.
//
// ДЕНЬГИ ОТСЮДА НЕ ВЫДАЮТСЯ. Сервис только читает балансы: выдача остаётся поездкой к
// работе — это правило игры, а не техническое ограничение. Поэтому в регистрации нет
// и не должно быть обработчика выдачи: единственная её точка — пикап работы.
//
// САМОРЕГИСТРАЦИЯ (как в JobDismissService): каждая работа зовёт registerWallet в
// конструкторе своей системы. Новая работа — одна строка регистрации.
//
// Кошельки грузятся из БД АСИНХРОННО на старте сессии, поэтому «есть ли деньги»
// сразу после логина спрашивать бесполезно: работа зовёт notifyLoaded, когда её
// баланс лёг в память, и подписчик (JobWalletSystem) решает, пора ли напоминать.
class JobWalletService final : public IService
{
  public:
    // Баланс кошелька работы у игрока онлайн (синхронно из кэша работы, без БД).
    using BalanceGetter = std::function<std::int64_t(int playerId)>;
    // Кошелёк работы догрузился из БД для этого игрока.
    using LoadedObserver = std::function<void(int playerId)>;

    struct Wallet
    {
        std::string name;  // utf-8, для игрока: «водитель автобуса»
        std::string where; // utf-8, где забирают: «депо автобусов»
        BalanceGetter balanceOf;
    };

    // Зарегистрировать кошелёк работы. Вызывается в конструкторе системы работы;
    // регистрация без геттера игнорируется.
    void registerWallet(std::string name, std::string where, BalanceGetter balanceOf);

    // Список кошельков в порядке регистрации. Ссылка стабильна: список наполняется
    // только на старте (конструкторы систем), в игре не меняется.
    const std::vector<Wallet> &wallets() const;

    // Сумма по всем кошелькам: 0 — забирать нечего.
    std::int64_t totalBalance(int playerId) const;

    // Работа сообщает, что её кошелёк для игрока загружен (зовётся из колбэка
    // async-загрузки). Оповещает subscribeLoaded.
    void notifyLoaded(int playerId);
    // Подписка на загрузку кошелька. Зовётся в конструкторе подписчика — до логинов.
    void subscribeLoaded(LoadedObserver observer);

  private:
    std::vector<Wallet> m_wallets;
    std::vector<LoadedObserver> m_loadedObservers;
};
