#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>

// Сервис местонахождения — единственный источник правды о позиции, интерьере и
// виртуальном мире игрока.
//
// Позиция в SA-MP клиент-авторитетна (клиент диктует её каждым sync-пакетом),
// поэтому сервис ведёт собственную «принятую» позицию: каждое заявление клиента
// проверяется на физическую достижимость (скорость с прошлого апдейта), и только
// правдоподобное движение становится правдой. Непровдоподобный скачок
// (телепорт-хак) откатывается и фиксируется как нарушение.
//
// Виртуальный мир — полностью серверный (клиент не может его сменить).
// Интерьер клиент меняет легально через enex-маркеры (это сопровождается
// телепортом) — на смену интерьера выдаётся разовый грейс позиции с
// рейт-лимитом, чтобы чит не легализовал телепорты спамом интерьеров.
//
// КОНТРАКТ: серверные перемещения — ТОЛЬКО через teleport()/setInterior()/
// setVirtualWorld() этого сервиса. Прямой player.setPosition() мимо сервиса
// валидатор посчитает читерским скачком и откатит. Инструментам, легально
// двигающим игрока в обход (редактор с FindZ-пробами), — setBypass(true).
class PlayerLocationService final : public IService
{
  public:
    // --- авторитетные операции ---
    void teleport(IPlayer &player, const Vector3 &position);
    void teleport(IPlayer &player, const Vector3 &position, unsigned interior, int virtualWorld);
    void setInterior(IPlayer &player, unsigned interior);
    void setVirtualWorld(IPlayer &player, int virtualWorld);

    // --- источник правды (принятая сервером, не сырая клиентская) ---
    Vector3 getPosition(int playerId) const;
    unsigned getInterior(int playerId) const;
    int getVirtualWorld(int playerId) const;

    // Отключить валидацию для игрока (редактор и прочие инструменты, легально
    // двигающие игрока в обход сервиса). Позиция продолжает приниматься —
    // источник правды остаётся свежим, но без проверок.
    void setBypass(int playerId, bool bypass);
    bool isBypassed(int playerId) const;

    // Растёт на каждом НЕнепрерывном изменении позиции (телепорт, спавн, грейс,
    // пауза, байпас). Потребители, считающие производные от позиции (скорость),
    // обязаны пропускать сэмплы через разрыв.
    std::uint32_t getDiscontinuity(int playerId) const;

    struct VerifyOutcome
    {
        bool teleportHack = false;
        std::string detail;
    };

    // --- вызывается PlayerLocationSystem ---
    VerifyOutcome verify(IPlayer &player, TimePoint now);
    void onSpawn(IPlayer &player);
    void onInteriorChange(IPlayer &player, unsigned newInterior, TimePoint now);
    void reset(int playerId);

  private:
    struct State
    {
        bool tracking = false;   // есть принятая позиция (первый апдейт её установит)
        bool bypass = false;     // валидация отключена (редактор)
        bool acceptNext = false; // разовый грейс: принять следующее заявление как есть
        Vector3 position{};      // принятая позиция — правда сервера
        unsigned interior = 0;
        int virtualWorld = 0;

        bool pendingTeleport = false; // ждём, пока клиент доедет до teleportTarget
        Vector3 teleportTarget{};
        TimePoint teleportAt;
        float arriveRadius = 30.0f; // адаптивный: доля от дальности прыжка, иначе
                                    // запоздавшие пакеты со старой позицией
                                    // засчитываются как «прибыл» при коротком откате

        TimePoint lastUpdate;        // для dt и детекта паузы
        TimePoint lastInteriorGrace; // рейт-лимит грейса по смене интерьера
        std::uint32_t discontinuity = 0; // счётчик разрывов непрерывности позиции
    };

    void forceTo(IPlayer &player, const Vector3 &position, TimePoint now);

    std::array<State, MAX_PLAYERS> m_state;
};
