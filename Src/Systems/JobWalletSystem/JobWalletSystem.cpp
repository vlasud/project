#include "Systems/JobWalletSystem/JobWalletSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const Colour INFO_COLOUR{120, 220, 255};
} // namespace

JobWalletSystem::JobWalletSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_jobWalletService(serviceRegister.getService<JobWalletService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "jobwallet", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            showWallets(player);
        },
        {}, "заработок по работам и выдача его на руки", PlayerCommandService::HelpCategory::Economy);

    // Свежая сессия — свежее напоминание (слот playerId переиспользуется).
    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const int playerId = player.getID();
            if (validPlayerId(playerId))
            {
                m_reminded[playerId] = false;
            }
        });

    // Кошельки грузятся из БД асинхронно и независимо друг от друга: спрашивать сумму
    // на старте сессии рано (там ещё нули), поэтому ждём сигнала от самих работ.
    m_jobWalletService.subscribeLoaded(
        [this](int playerId)
        {
            onWalletLoaded(playerId);
        });
}

void JobWalletSystem::onWalletLoaded(int playerId)
{
    if (!validPlayerId(playerId) || m_reminded[playerId])
    {
        return;
    }
    // Нулевая сумма — не повод молчать навсегда: остальные кошельки ещё грузятся, и
    // напоминание уйдёт на той загрузке, которая даст плюс. Если денег нет нигде,
    // игрок так ничего и не увидит — как и задумано.
    if (m_jobWalletService.totalBalance(playerId) <= 0)
    {
        return;
    }
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    m_reminded[playerId] = true;
    player->sendClientMessage(INFO_COLOUR, u("Вы можете забрать заработанные деньги. Подробнее /jobwallet"));
}

void JobWalletSystem::showWallets(IPlayer &player)
{
    const int playerId = player.getID();
    const std::vector<JobWalletService::Wallet> &wallets = m_jobWalletService.wallets();
    if (wallets.empty())
    {
        return; // работ с кошельками в сборке нет — показывать нечего
    }

    // Окно СПРАВОЧНОЕ: деньги отсюда не выдаются, за ними едут к работе. Поэтому одна
    // кнопка «Закрыть» и колонка «где забрать» — без неё список бесполезен.
    //
    // Показываем ВСЕ работы, в том числе с нулём: недоступность не прячет строку
    // (правило проекта), заодно видно, где заработок вообще бывает.
    std::string body = "Работа\tВ кошельке\tГде забрать\n";
    for (const JobWalletService::Wallet &wallet : wallets)
    {
        body += fmt::format("{}\t${}\t{}\n", wallet.name, wallet.balanceOf(playerId), wallet.where);
    }
    body.pop_back(); // хвостовой '\n' дал бы пустую строку-фантом в tablist

    m_dialogService.show(player,
                         makeDialog(DialogStyle_TABLIST_HEADERS,
                                    fmt::format("Заработок по работам — всего ${}",
                                                m_jobWalletService.totalBalance(playerId)),
                                    body, "Закрыть", ""),
                         [](DialogResponse, int, StringView)
                         {
                         });
}
