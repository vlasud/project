#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <functional>

class ClassSelectionSystem;
class AntiCheatService;

// Класс-селекшн отключён полностью: выбрать класс игрок не может никогда.
//
// Единственный подписчик событий классов — ClassSelectionSystem — маршрутизирует
// их сюда. Гарантии сервиса:
//  * запрос класса всегда отклоняется (листать классы нельзя);
//  * дефолтные кнопки экрана (стрелки ◄ ► и Spawn) блокируются с сообщением;
//  * легальный авто-респаун после смерти ПРОПУСКАЕТСЯ (клиент сам шлёт
//    RequestSpawn после wasted-экрана — без этого кнопки мелькают на каждом
//    респауне);
//  * подделанные RPC валидируются: вход в класс-селекшн легален только при
//    подключении (до первого спавна) или после зафиксированной СЕРВЕРОМ смерти
//    (F4). RequestClass от живого игрока — это попытка получить телепорт+хил
//    через respawn: отклоняется и пишется SpawnHack в журнал античита. Ранний
//    RequestSpawn (раньше клиентского wasted-экрана) тоже отклоняется.
//
// Каждый легальный ВХОД в класс-селекшн отдаётся бизнес-обработчику
// setEntryHandler, который обязан увести игрока с экрана (respawn).
class ClassSelectionService final : public IService
{
    friend ClassSelectionSystem;

  public:
    using EntryHandler = std::function<void(IPlayer &)>;

    // Обработчик входа в класс-селекшн. Один на весь сервер (бизнес-флоу входа).
    void setEntryHandler(EntryHandler handler);

  private:
    // Вызываются ClassSelectionSystem.
    void initialize(AntiCheatService *antiCheat);
    bool handleRequestClass(IPlayer &player, TimePoint now); // возвращаемое значение — ответ клиенту
    bool handleRequestSpawn(IPlayer &player, TimePoint now);
    void handleDeath(IPlayer &player, TimePoint now); // смерть, зафиксированная сервером
    void handleSpawn(IPlayer &player);
    void resetPlayer(int playerId);

    struct Slot
    {
        bool inSelection = false;  // на экране класс-селекшна (до спавна)
        bool everSpawned = false;  // хоть раз спавнился за это подключение
        bool deathPending = false; // умер и ещё не отреспавнился
        TimePoint diedAt{};
    };

    EntryHandler m_entryHandler;
    AntiCheatService *m_antiCheat = nullptr;
    std::array<Slot, MAX_PLAYERS> m_slots;
};
