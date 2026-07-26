#include "Systems/Core/NicknameSystem/NicknameSystem.h"

#include "Log/LogManager.h"
#include "Utils/Encoding/Encoding.h"
#include <chrono>
#include <fmt/format.h>

using namespace std::chrono_literals;

namespace
{
// Запас на чтение диалога, если игрок не закрывает его кнопкой. Заспавниться и
// играть за это время он теоретически может — кик неминуем в любом случае.
constexpr Milliseconds KICK_TIMEOUT{15s};

std::string sv(StringView view)
{
    return std::string(view.data(), view.size());
}
} // namespace

NicknameSystem::NicknameSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_nicknameService(serviceRegister.getService<NicknameService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_timerService(serviceRegister.getService<TimerService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
}

void NicknameSystem::reject(IPlayer &player, const std::string &reason)
{
    LogManager::log(Warning, fmt::format("NicknameSystem: rejecting '{}': {}", sv(player.getName()), reason));

    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = u("Недопустимый ник");
    dialog.body = u(fmt::format("Ваш ник: {}\n"
                                "Причина отказа: {}\n\n"
                                "Мы дорожим атмосферой сервера, поэтому ники всех игроков\n"
                                "выдержаны в едином стиле — Имя_Фамилия, например Ivan_Ivanov:\n"
                                "- только латинские буквы и один символ _\n"
                                "- обе части с заглавной буквы, минимум 2 буквы каждая\n"
                                "- длина 5-20 символов\n\n"
                                "Пожалуйста, измените ник в настройках — и добро пожаловать!",
                                sv(player.getName()), reason));
    dialog.leftButton = u("Закрыть");

    // Кик по закрытию диалога; страховочный таймер — если диалог игнорируют.
    // Двойного кика нет: после первого игрок отключается, пер-плеерный таймер
    // отменяется автоматически, а запоздалый ответ диалога отбросит роутер.
    m_dialogService.show(player, dialog,
                         [&player](DialogResponse, int, StringView)
                         {
                             player.kick();
                         });
    m_timerService.setPlayerTimeout(player, KICK_TIMEOUT,
                                    [](IPlayer &p)
                                    {
                                        p.kick();
                                    });
}

void NicknameSystem::onPlayerConnect(IPlayer &player)
{
    const NicknameService::Verdict verdict = m_nicknameService.validate(player.getName());
    if (verdict != NicknameService::Verdict::Ok)
    {
        reject(player, NicknameService::describe(verdict));
    }
}
