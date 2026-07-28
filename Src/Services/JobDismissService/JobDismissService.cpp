#include "Services/JobDismissService/JobDismissService.h"

#include "Log/LogManager.h"
#include <utility>

void JobDismissService::registerJob(std::string name, std::string warning, WorkingPredicate isWorking,
                                    DismissHandler dismiss)
{
    if (!isWorking || !dismiss)
    {
        LogManager::log(Error, "JobDismissService: job '" + name + "' registered without handlers, ignored");
        return;
    }
    // reserve не делаем: регистраций единицы и все на старте; указатели на Job
    // раздаём ПОСЛЕ наполнения (в игре список неизменен), поэтому перевыделение
    // вектора живых указателей не задевает.
    m_jobs.push_back(Job{std::move(name), std::move(warning), std::move(isWorking), std::move(dismiss)});
}

const JobDismissService::Job *JobDismissService::currentJob(int playerId) const
{
    for (const Job &job : m_jobs)
    {
        if (job.isWorking(playerId))
        {
            return &job;
        }
    }
    return nullptr;
}
