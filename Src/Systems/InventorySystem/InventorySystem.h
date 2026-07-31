#pragma once

#include "Macro.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/InventoryService/InventoryService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>

// Проводник вещей: лайфцикл слота, персист в БД и дев-выдача предметов.
//
//  * коннект/дисконнект — сброс слота InventoryService (как
//    WeaponProficiencySystem чистит свой), чтобы вещи одного аккаунта не утекли
//    следующему игроку в том же playerId-слоте;
//  * старт сессии (subscribeStart) — async-загрузка строк `player_items` по
//    account_id с serial-guard (как WeaponProficiencySystem::loadProficiency):
//    запрос на воркере вычитывает строки в владеющий вектор, на главном потоке
//    сверяется serial и количества раскладываются через InventoryService::loadItems
//    (клампит к maxStack, незарегистрированные/мусор отбрасывает);
//  * персист (subscribeSave) — REPLACE снимка вещей в `player_items` по
//    account_id одним throwQuery: удаляем все строки аккаунта и вставляем текущие
//    ненулевые. Идемпотентен, поэтому в save-канал: зовётся на конце сессии (до
//    teardown) И периодически автосейвом онлайн-игроков (см. Docs/Autosave.md).
//    НЕ при каждом изменении в сессии (для дев-выдачи это ок; будущая покупка
//    должна форсить сохранение или перейти на write-through — см. Docs/Inventory.md).
//
// Дев-выдача (конвенция «дев-тулинг = ОДНА команда → диалог-меню», как /edev):
// /idev (только разработчик, admin level 6) — меню зарегистрированных предметов →
// выбор → ввод количества → InventoryService::add вызвавшему. Ввод количества
// клиентский: валидируется (целое > 0, разумный предел).
//
// Эффектами предметов система НЕ занимается — это системы конкретных предметов
// (MedkitSystem). Здесь только хранение/персист/выдача.
class InventorySystem : public BaseSystem, public PlayerConnectEventHandler
{
  public:
    InventorySystem(ICore &core, const ServiceRegister &serviceRegister);

    void onPlayerConnect(IPlayer &player) override;
    void onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason) override;

  private:
    void loadItems(IPlayer &player, const PlayerSessionService::Session &session);
    void persistItems(IPlayer &player, const PlayerSessionService::Session &session);

    // Дев-меню /idev.
    // Инвентарь игрока (/inv): список НЕНУЛЕВЫХ предметов, выбор строки применяет
    // предмет. Порядок строк = порядок в снимке, на нём держится разбор listItem.
    void showInventory(IPlayer &player);
    void showDevMenu(IPlayer &player);
    void showDevAmountInput(IPlayer &player, int itemType);

    InventoryService &m_inventoryService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;

    // true — async-загрузка вещей из БД для слота успешно завершилась. Пока false
    // (загрузка не дошла: сбой/хиккап БД, дисконнект до колбэка), на конце сессии
    // НЕ персистим: иначе пустой слот затёр бы реальные вещи в БД.
    std::array<bool, MAX_PLAYERS> m_loaded{};
};
