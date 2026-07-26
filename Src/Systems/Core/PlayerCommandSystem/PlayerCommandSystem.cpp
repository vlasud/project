#include "Systems/Core/PlayerCommandSystem/PlayerCommandSystem.h"
#include "Utils/Encoding/Encoding.h"

namespace
{
// Отказ прав и реальная неизвестная команда дают идентичный ответ (решение
// геймдизайнера): тот, кому команда недоступна, не отличит её отсутствие от
// отсутствия прав — существование команды не палится.
const std::string ERROR_MESSAGE = Encoding::utf8Tocp1251("Неизвестная команда. Введите /help");
} // namespace

PlayerCommandSystem::PlayerCommandSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_commandService(serviceRegister.getService<PlayerCommandService>()),
      m_adminService(serviceRegister.getService<AdminService>()),
      m_factionService(serviceRegister.getService<FactionService>())
{
    listen(core.getPlayers().getPlayerTextDispatcher(), this);
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);

    // Общий резолвер прав: команды декларируют PermissionSpec, здесь — единственная
    // трансляция в серверные факты. AdminLevel гейтится getEffectiveLevel (0 до
    // /alogin), фракционные — членством/маской (НЕ числовой лестницей).
    m_commandService.setPermissionResolver(
        [this](IPlayer &player, const PermissionSpec &spec) -> bool
        {
            const int id = player.getID();
            switch (spec.kind)
            {
            case PermissionSpec::Kind::None:
                return true;
            case PermissionSpec::Kind::AdminLevel:
                return m_adminService.getEffectiveLevel(id) >= spec.adminLevel;
            case PermissionSpec::Kind::FactionMember:
                return m_factionService.getMemberFaction(id) == spec.factionId;
            case PermissionSpec::Kind::FactionPermission:
                return m_factionService.getMemberFaction(id) == spec.factionId &&
                       (spec.factionMask == 0 ? true : m_factionService.hasPermission(id, spec.factionMask));
            }
            return false;
        });
}

bool PlayerCommandSystem::onPlayerCommandText(IPlayer &player, StringView message)
{
    const bool result = m_commandService.dispatch(player, message);
    if (!result)
    {
        player.sendClientMessage(Colour::White(), ERROR_MESSAGE);
    }
    return true;
}

void PlayerCommandSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_commandService.reset(player.getID());
}
