#pragma once

#include "../../Macro.h"
#include "../IService.h"
#include "types.hpp"
#include <array>
#include <chrono>
#include <string>

// Сервис чата: мут и антиспам.
//
// Мут — API для блокировки чата игроку (админ-командами, репорт-системой и т.д.).
// Пока сессионный (живёт до выхода игрока); персист в БД — на стороне будущей
// админ-системы.
//
// Антиспам в tryChat() трёхступенчатый:
//  * токен-бакет темпа: burst 3 сообщения, дальше 1 на 1.5 с;
//  * повтор одинакового текста в течение 10 с отклоняется;
//  * накопленные отказы (3 за 30 с) — автомут на 30 с.
class PlayerChatService final : public IService
{
  public:
    enum class Block
    {
        None,      // можно говорить
        Muted,     // в муте (secondsLeft заполнен)
        TooFast,   // превышен темп
        Duplicate, // повтор того же текста
    };

    struct Check
    {
        Block block = Block::None;
        int secondsLeft = 0; // остаток мута для Block::Muted (в т.ч. автомута)
    };

    // Полная проверка сообщения; при успехе регистрирует его (тратит токен,
    // запоминает текст). Вызывается чат-системой на каждое сообщение.
    Check tryChat(int playerId, StringView message, TimePoint now);

    // --- мут ---
    void mute(int playerId, std::chrono::seconds duration);
    void unmute(int playerId);
    bool isMuted(int playerId) const;
    int muteSecondsLeft(int playerId) const;

    void reset(int playerId);

  private:
    struct State
    {
        TimePoint muteUntil; // epoch — не в муте

        float tokens = 3.0f; // токен-бакет темпа сообщений
        TimePoint lastRefill;

        std::string lastMessage; // для детекта повтора
        TimePoint lastMessageAt;

        int strikes = 0; // отказы антиспама; копятся к автомуту
        TimePoint lastStrike;
    };

    void addStrike(State &st, TimePoint now);

    std::array<State, MAX_PLAYERS> m_state;
};
