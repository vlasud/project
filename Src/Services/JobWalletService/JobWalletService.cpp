#include "Services/JobWalletService/JobWalletService.h"

#include "Log/LogManager.h"
#include <utility>

void JobWalletService::registerWallet(std::string name, std::string where, BalanceGetter balanceOf)
{
    if (!balanceOf)
    {
        LogManager::log(Error, "JobWalletService: wallet '" + name + "' registered without balance getter, ignored");
        return;
    }
    m_wallets.push_back(Wallet{std::move(name), std::move(where), std::move(balanceOf)});
}

const std::vector<JobWalletService::Wallet> &JobWalletService::wallets() const
{
    return m_wallets;
}

std::int64_t JobWalletService::totalBalance(int playerId) const
{
    std::int64_t total = 0;
    for (const Wallet &wallet : m_wallets)
    {
        const std::int64_t balance = wallet.balanceOf(playerId);
        if (balance > 0)
        {
            total += balance; // отрицательного баланса у кошельков не бывает, но суммируем только плюс
        }
    }
    return total;
}

void JobWalletService::notifyLoaded(int playerId)
{
    for (const LoadedObserver &observer : m_loadedObservers)
    {
        observer(playerId);
    }
}

void JobWalletService::subscribeLoaded(LoadedObserver observer)
{
    if (observer)
    {
        m_loadedObservers.push_back(std::move(observer));
    }
}
