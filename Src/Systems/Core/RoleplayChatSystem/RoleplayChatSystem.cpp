#include "Systems/Core/RoleplayChatSystem/RoleplayChatSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "fmt/format.h"
#include <random>

namespace
{
constexpr float RP_RADIUS = 20.0f;
constexpr size_t RP_BUFFER_SIZE = 128 + 1;
// Текст режется по байтам (cp1251 однобайтовый), чтобы итог влез в буфер клиента
// с учётом префикса "* ", имени (макс. 24), пробелов и обрамления "(( ... ))".
constexpr int RP_MAX_TEXT_LENGTH = 80;

const Colour RP_COLOUR = Colour::FromRGBA(0xC2A2DAFF); // фиолетовый RP

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// Исход /try: 50/50. Генератор thread_local, сидится один раз — без аллокаций
// и пересоздания на вызов.
bool trySucceeds()
{
    thread_local std::mt19937 rng{std::random_device{}()};
    thread_local std::bernoulli_distribution coin(0.5);
    return coin(rng);
}

// Готовит клиентский текст к показу: режет по байтам (cp1251 однобайтовый),
// обезвреживает '{'/'}' (иначе игрок подделает цвет через {RRGGBB} — не зависим
// от серверного chat_input_filter) и выкидывает управляющие байты (\n, \t и пр.).
// Пишет в стековый буфер, без аллокаций.
StringView sanitizeEmote(StringView text, char (&out)[RP_MAX_TEXT_LENGTH + 1])
{
    const size_t n = std::min<size_t>(text.size(), RP_MAX_TEXT_LENGTH);
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '{')
            out[i] = '(';
        else if (c == '}')
            out[i] = ')';
        else if (c < 0x20)
            out[i] = ' ';
        else
            out[i] = text[i];
    }
    return StringView(out, n);
}
} // namespace

RoleplayChatSystem::RoleplayChatSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_gridService(serviceRegister.getService<GridService>()),
      m_chatService(serviceRegister.getService<PlayerChatService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>())
{
    m_listeners.reserve(64);

    auto &commands = serviceRegister.getService<PlayerCommandService>();

    // /me — действие от лица персонажа: "* Имя машет рукой".
    commands.add("me", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     // Мут — общий барьер с чатом: через эмоуты мут не обойти.
                     if (!passChatGate(player))
                         return;

                     char textBuf[RP_MAX_TEXT_LENGTH + 1];
                     const StringView text = sanitizeEmote(args.getString(0), textBuf);

                     char buffer[RP_BUFFER_SIZE] = {0};
                     const auto formatted =
                         fmt::format_to_n(buffer, RP_BUFFER_SIZE - 1, "* {} {}", player.getName(), text);
                     broadcast(player, StringView(buffer, std::min<size_t>(formatted.size, RP_BUFFER_SIZE - 1)));
                 },
                 {}, "описать своё действие в ролевом чате", PlayerCommandService::HelpCategory::ChatRP);

    // /do — описание окружения/состояния: "* дверь приоткрыта ((Имя))".
    commands.add("do", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     if (!passChatGate(player))
                         return;

                     char textBuf[RP_MAX_TEXT_LENGTH + 1];
                     const StringView text = sanitizeEmote(args.getString(0), textBuf);

                     char buffer[RP_BUFFER_SIZE] = {0};
                     const auto formatted =
                         fmt::format_to_n(buffer, RP_BUFFER_SIZE - 1, "* {} (({}))", text, player.getName());
                     broadcast(player, StringView(buffer, std::min<size_t>(formatted.size, RP_BUFFER_SIZE - 1)));
                 },
                 {}, "описать обстановку или состояние вокруг", PlayerCommandService::HelpCategory::ChatRP);

    // /try — попытка действия с исходом 50/50: "* Имя дёргает дверь ((удачно))".
    commands.add("try", {{PlayerCommandService::Param::String, "текст"}},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
                 {
                     if (!passChatGate(player))
                         return;

                     char textBuf[RP_MAX_TEXT_LENGTH + 1];
                     const StringView text = sanitizeEmote(args.getString(0), textBuf);
                     const std::string result = trySucceeds() ? u("удачно") : u("неудачно");

                     char buffer[RP_BUFFER_SIZE] = {0};
                     const auto formatted = fmt::format_to_n(buffer, RP_BUFFER_SIZE - 1, "* {} {} (({}))",
                                                             player.getName(), text, result);
                     broadcast(player, StringView(buffer, std::min<size_t>(formatted.size, RP_BUFFER_SIZE - 1)));
                 },
                 {}, "описать действие с исходом удача/провал", PlayerCommandService::HelpCategory::ChatRP);
}

bool RoleplayChatSystem::passChatGate(IPlayer &player)
{
    // Только мут — это модерация: заглушённый игрок не должен эмоутить. Флуд и
    // повтор тут НЕ учитываем (не зовём tryChat) — антифлуд команд корневой,
    // в PlayerCommandService (см. Docs/CommandFlood.md).
    const int playerId = player.getID();
    if (m_chatService.isMuted(playerId))
    {
        player.sendClientMessage(
            Colour::White(), u(fmt::format("Чат заблокирован. Осталось: {} сек", m_chatService.muteSecondsLeft(playerId))));
        return false;
    }
    return true;
}

void RoleplayChatSystem::broadcast(IPlayer &author, StringView line)
{
    const int authorId = author.getID();
    const Vector3 position = m_locationService.getPosition(authorId);
    const int virtualWorld = m_locationService.getVirtualWorld(authorId);
    const unsigned interior = m_locationService.getInterior(authorId);

    m_gridService.queryRadius(position, RP_RADIUS, gridMask(GridEntityType::Player), m_listeners);

    for (const GridService::Result &listener : m_listeners)
    {
        // Эмоут слышат игроки того же виртуального мира И интерьера (включая
        // автора): сквозь стены в соседнюю «комнату» с тем же VW не проходит.
        if (m_locationService.getVirtualWorld(listener.id) != virtualWorld ||
            m_locationService.getInterior(listener.id) != interior)
        {
            continue;
        }

        IPlayer *target = m_core.getPlayers().get(listener.id);
        if (!target)
        {
            continue;
        }

        target->sendClientMessage(RP_COLOUR, line);
    }
}
