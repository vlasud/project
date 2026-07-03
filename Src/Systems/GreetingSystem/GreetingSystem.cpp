#include "Systems/GreetingSystem/GreetingSystem.h"

#include "Utils/Encoding/Encoding.h"
#include <chrono>

namespace
{
constexpr std::chrono::milliseconds GREETING_TIME{5000};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

GreetingSystem::GreetingSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_screenNotice(serviceRegister.getService<ScreenNoticeService>())
{
    // Сессия стартует после успешного логина/регистрации — самый ранний момент,
    // когда игрок уже опознан. Ник — IPlayer::getName(), строго латиница
    // Имя_Фамилия (валидирован NicknameService раньше), безопасен для textdraw
    // без доп. санитизации.
    auto &sessionService = serviceRegister.getService<PlayerSessionService>();
    sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &)
        {
            const std::string text = u("WELCOME, ") + player.getName().to_string();
            m_screenNotice.show(player, text, GREETING_TIME, Colour::White());
        });
}
