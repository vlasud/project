#include "Systems/JobDismissSystem/JobDismissSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/MakeDialog.h"
#include "Utils/Encoding/Encoding.h"
#include <fmt/format.h>

namespace
{
const Colour ERROR_COLOUR{255, 90, 90};
} // namespace

JobDismissSystem::JobDismissSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_jobDismissService(serviceRegister.getService<JobDismissService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "stopjob", {},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
        {
            onStopJob(player);
        },
        {}, "прервать текущую работу", PlayerCommandService::HelpCategory::Economy);
}

void JobDismissSystem::onStopJob(IPlayer &player)
{
    const int playerId = player.getID();
    const JobDismissService::Job *job = m_jobDismissService.currentJob(playerId);
    if (!job)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете"));
        return;
    }

    const std::string body = fmt::format("Вы работаете: {}\n\n{}\n\nПрервать смену прямо сейчас?", job->name,
                                         job->warning);
    m_dialogService.show(player, makeDialog(DialogStyle_MSGBOX, "Прервать работу", body, "Прервать", "Отмена"),
                         [this, playerId](DialogResponse response, int, StringView)
                         {
                             IPlayer *worker = m_core.getPlayers().get(playerId);
                             if (!worker || response != DialogResponse_Left)
                             {
                                 return;
                             }
                             // Ре-валидация на клике: за время диалога смена могла кончиться сама
                             // (смерть, пропажа транспорта, анти-AFK) или смениться на другую.
                             const JobDismissService::Job *current = m_jobDismissService.currentJob(playerId);
                             if (!current)
                             {
                                 worker->sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете"));
                                 return;
                             }
                             current->dismiss(*worker);
                         });
}
