#include "Systems/PaymentSystem/PaymentSystem.h"

#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include "fmt/format.h"
#include "types.hpp"
#include <chrono>
#include <limits>

namespace
{
constexpr float PAY_RADIUS = 5.0f; // дистанция передачи из рук в руки
constexpr size_t PAY_BUFFER_SIZE = 128 + 1;
const Colour PAY_COLOUR = Colour::FromRGBA(0x33AA33FF); // читаемый зелёный
// Потолок баланса: HUD money на клиенте — signed int32, выше уедет в минус.
constexpr unsigned long long MONEY_MAX = static_cast<unsigned long long>(std::numeric_limits<int>::max());
constexpr std::chrono::milliseconds PAY_COOLDOWN{1000}; // антифлуд: не чаще раза в секунду на игрока

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}
} // namespace

PaymentSystem::PaymentSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>())
{
    serviceRegister.getService<PlayerCommandService>().add(
        "pay", {{PlayerCommandService::Param::Int, "id игрока"}, {PlayerCommandService::Param::Int, "сумма"}},
        [this](IPlayer &player, const PlayerCommandService::CommandArgs &args)
        {
            const int payerId = player.getID();
            const int targetId = args.getInt(0);

            // Антифлуд: успешный перевод шлёт строку и получателю, поэтому темп
            // ограничиваем, чтобы нельзя было засыпать чужой чат переводами по 1$.
            const TimePoint now = std::chrono::steady_clock::now();
            if (now - m_lastPay[payerId] < PAY_COOLDOWN)
            {
                player.sendClientMessage(PAY_COLOUR, u("Не так быстро"));
                return;
            }

            // get() сам отсекает невалидный/неактивный id (в т.ч. отрицательный).
            IPlayer *target = m_core.getPlayers().get(targetId);
            if (!target || targetId == payerId)
            {
                player.sendClientMessage(PAY_COLOUR, u("Игрок не найден"));
                return;
            }

            // Сумма приходит как int; отрицательную/нулевую отвергаем (иначе кража
            // через отрицательный перевод). Только после этого каст в unsigned.
            const int rawAmount = args.getInt(1);
            if (rawAmount <= 0)
            {
                player.sendClientMessage(PAY_COLOUR, u("Сумма должна быть больше нуля"));
                return;
            }
            const unsigned long long amount = static_cast<unsigned long long>(rawAmount);

            // Баланс — серверный (НЕ клиентский IPlayer::getMoney): клиенту про
            // деньги не верим.
            const unsigned long long payerBalance = m_moneyService.getMoney(payerId);
            if (payerBalance < amount)
            {
                player.sendClientMessage(PAY_COLOUR, u("Недостаточно денег"));
                return;
            }

            // «Рядом»: тот же виртуальный мир и интерьер, дистанция по квадрату
            // (sqrt не нужен) — всё по серверным позициям, не по клиентским.
            if (m_locationService.getVirtualWorld(targetId) != m_locationService.getVirtualWorld(payerId) ||
                m_locationService.getInterior(targetId) != m_locationService.getInterior(payerId))
            {
                player.sendClientMessage(PAY_COLOUR, u("Игрок слишком далеко"));
                return;
            }
            const Vector3 delta = m_locationService.getPosition(targetId) - m_locationService.getPosition(payerId);
            const float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            if (distSq > PAY_RADIUS * PAY_RADIUS)
            {
                player.sendClientMessage(PAY_COLOUR, u("Игрок слишком далеко"));
                return;
            }

            // Зачисление не должно увести баланс получателя за потолок (иначе HUD
            // money, signed int32, уйдёт в минус). Проверка без unsigned-underflow.
            const unsigned long long targetBalance = m_moneyService.getMoney(targetId);
            if (targetBalance >= MONEY_MAX || amount > MONEY_MAX - targetBalance)
            {
                player.sendClientMessage(PAY_COLOUR, u("Перевод невозможен"));
                return;
            }

            // Списание и зачисление абсолютным setMoney в одном обработчике —
            // перевод синхронный и атомарный, HUD обоих участников = новый баланс.
            m_moneyService.setMoney(player, payerBalance - amount);
            m_moneyService.setMoney(*target, targetBalance + amount);
            m_lastPay[payerId] = now;

            // Прерываемая анимация передачи: игрок выходит из неё движением, сервер
            // не переустанавливает (interruptible).
            m_animationService.play(player, AnimationData(4.0f, false, false, false, false, 0, "DEALER", "DEALER_DEAL"),
                                    true);

            // "Lo_Vlasud[22] передал Ivan_Ivanov[23] 100$"
            char buffer[PAY_BUFFER_SIZE] = {0};
            const auto formatted =
                fmt::format_to_n(buffer, PAY_BUFFER_SIZE - 1, "{}[{}] {} {}[{}] {}$", player.getName(), payerId,
                                 u("передал"), target->getName(), targetId, amount);
            const StringView line(buffer,
                                  formatted.size < PAY_BUFFER_SIZE - 1 ? formatted.size : PAY_BUFFER_SIZE - 1);

            player.sendClientMessage(PAY_COLOUR, line);
            target->sendClientMessage(PAY_COLOUR, line);
        });
}
