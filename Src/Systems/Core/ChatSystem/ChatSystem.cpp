#include "Systems/Core/ChatSystem/ChatSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "fmt/format.h"
#include "types.hpp"
#include <chrono>

namespace
{
constexpr float CHAT_RADIUS = 20.0f;
constexpr size_t CHAT_BUFFER_SIZE = 128 + 1;
constexpr int MAX_MESSAGE_LENGTH = 93; // "- текст : Имя[id]" укладывается в 128

// Цвет тускнеет с расстоянием: ближе — белее. Пороги — квартили радиуса,
// сравнение по квадрату дистанции (sqrt не нужен).
struct ColourBand
{
    float maxDistSq;
    Colour colour;
};
const ColourBand COLOUR_BANDS[] = {
    {5.0f * 5.0f, Colour::FromRGBA(0xD3D3D3FF)},   // LightGray
    {10.0f * 10.0f, Colour::FromRGBA(0xC0C0C0FF)}, // Silver
    {15.0f * 15.0f, Colour::FromRGBA(0xA9A9A9FF)}, // DarkGray
    {CHAT_RADIUS * CHAT_RADIUS, Colour::FromRGBA(0xA9A9A9FF)},
};

Colour colourForDistance(float distSq)
{
    for (const ColourBand &band : COLOUR_BANDS)
    {
        if (distSq <= band.maxDistSq)
            return band.colour;
    }
    return COLOUR_BANDS[3].colour;
}

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

ChatSystemSystem::ChatSystemSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gridService(serviceRegister.getService<GridService>()),
      m_chatService(serviceRegister.getService<PlayerChatService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>())
{
    core.getPlayers().getPlayerTextDispatcher().addEventHandler(this);
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    m_listeners.reserve(64);

    // Временные команды модерации — до появления админ-системы с правами.
    auto &commands = serviceRegister.getService<PlayerCommandService>();

    commands.add("mute", {{PlayerCommandService::Param::Int, "id игрока"}, {PlayerCommandService::Param::Int, "секунды"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     IPlayer *target = m_core.getPlayers().get(args.getInt(0));
                     if (!target)
                     {
                         player.sendClientMessage(Colour::White(), u("Игрок не найден"));
                         return;
                     }
                     int seconds = args.getInt(1);
                     if (seconds < 1 || seconds > 86400)
                         seconds = 60;
                     m_chatService.mute(target->getID(), std::chrono::seconds(seconds));
                     player.sendClientMessage(Colour::White(),
                                              u(fmt::format("Игрок {} замучен на {} сек", args.getInt(0), seconds)));
                     target->sendClientMessage(Colour::White(),
                                               u(fmt::format("Вам выдан мут на {} сек", seconds)));
                 },
                 {}, "замутить игрока в чате на N секунд", PlayerCommandService::HelpCategory::Hidden);

    commands.add("unmute", {{PlayerCommandService::Param::Int, "id игрока"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     IPlayer *target = m_core.getPlayers().get(args.getInt(0));
                     if (!target)
                     {
                         player.sendClientMessage(Colour::White(), u("Игрок не найден"));
                         return;
                     }
                     m_chatService.unmute(target->getID());
                     player.sendClientMessage(Colour::White(), u(fmt::format("Мут снят с {}", args.getInt(0))));
                     target->sendClientMessage(Colour::White(), u("Мут снят"));
                 },
                 {}, "снять мут с игрока", PlayerCommandService::HelpCategory::Hidden);
}

bool ChatSystemSystem::onPlayerText(IPlayer &player, StringView message)
{
    const int playerId = player.getID();
    const TimePoint now = std::chrono::steady_clock::now();

    // Мут и антиспам — до любой работы с сообщением.
    const PlayerChatService::Check check = m_chatService.tryChat(playerId, message, now);
    switch (check.block)
    {
    case PlayerChatService::Block::Muted:
        player.sendClientMessage(Colour::White(),
                                 u(fmt::format("Чат заблокирован. Осталось: {} сек", check.secondsLeft)));
        return false;
    case PlayerChatService::Block::TooFast:
        player.sendClientMessage(Colour::White(), u("Не так быстро! Подождите немного"));
        return false;
    case PlayerChatService::Block::Duplicate:
        player.sendClientMessage(Colour::White(), u("Не повторяйтесь"));
        return false;
    case PlayerChatService::Block::None:
        break;
    }

    // Прерываемая анимация разговора: игрок выходит из неё движением, сервер
    // не переустанавливает (interruptible). В транспорте не проигрываем.
    if (m_stateService.getState(playerId) == PlayerState_OnFoot)
    {
        m_animationService.play(player, AnimationData(4.1f, false, false, false, false, 0, "PED", "IDLE_CHAT"), true);
    }

    const Vector3 position = m_locationService.getPosition(playerId);
    const int virtualWorld = m_locationService.getVirtualWorld(playerId);

    m_gridService.queryRadius(position, CHAT_RADIUS, gridMask(GridEntityType::Player), m_listeners);

    message.remove_suffix(std::max<int>(0, static_cast<int>(message.size()) - MAX_MESSAGE_LENGTH));

    // "- привет, как дела? : Lo_Vlasud[1000]"
    char buffer[CHAT_BUFFER_SIZE] = {0};
    const auto formatted = fmt::format_to_n(buffer, CHAT_BUFFER_SIZE - 1, "- {} : {}[{}]", message, player.getName(),
                                            playerId);
    const StringView line(buffer, formatted.size < CHAT_BUFFER_SIZE - 1 ? formatted.size : CHAT_BUFFER_SIZE - 1);

    for (const GridService::Result &listener : m_listeners)
    {
        // Слышат только игроки того же виртуального мира.
        if (m_locationService.getVirtualWorld(listener.id) != virtualWorld)
        {
            continue;
        }

        IPlayer *target = m_core.getPlayers().get(listener.id);
        if (!target)
        {
            continue;
        }

        target->sendClientMessage(colourForDistance(listener.distSq), line);
    }

    return false; // дефолтный глобальный чат подавлен
}

void ChatSystemSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_chatService.reset(player.getID());
}
