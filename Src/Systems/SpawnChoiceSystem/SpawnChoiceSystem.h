#pragma once

#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/FactionService/FactionService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Services/PlayerSpawnService/PlayerSpawnService.h"
#include "Services/SpawnChoiceService/SpawnChoiceService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"

// Привод фичи выбора точки спавна (/setspawn). Бизнес-фича, НЕ Core.
//
// Выбор главнее фракции: игрок ВСЕГДА может выбрать точку спавна, независимо от
// членства. SpawnChoiceSystem — ЕДИНСТВЕННЫЙ писатель точки спавна (FactionSystem
// её больше НЕ форсит). Дефолт для всех, кто не выбирал, — ЖД вокзал.
//
//  * /setspawn — открывает всем диалог LIST из трёх пунктов: ЖД вокзал / Дом /
//    Место работы (база фракции);
//  * по старту сессии грузит выбор аккаунта из БД (serial-guard) в
//    SpawnChoiceService и применяет (applySpawn) — авторизационный спавн приходит
//    уже в выбранную точку;
//  * applySpawn ВСЕГДА резолвит выбор в SpawnPoint от СЕРВЕРНЫХ фактов в момент
//    применения (дом из HouseService, база — из FactionService); если источник
//    пропал/недоступен — фолбэк на вокзал;
//  * перерезолв на смене членства: подписка на FactionService::subscribeMemberChange
//    зовёт applySpawn. Это (1) снимает гонку двух async-загрузок на логине
//    (членство и выбор грузятся раздельно — какой бы колбэк ни был вторым, у него
//    уже есть и членство, и выбор), (2) перерезолвит экс-члена с выбором Work на
//    вокзал при выходе из орга (иначе остался бы со спавном в приватном vw базы);
//  * выбор персистится write-through в SpawnChoiceService (REPLACE player_spawn).
//
// Спавн задаётся ТОЛЬКО через PlayerSpawnService::setSpawn (единственный источник
// правды о спавне), команда НЕ телепортирует игрока сейчас — лишь настраивает
// будущие спавны.
class SpawnChoiceSystem : public BaseSystem
{
  public:
    SpawnChoiceSystem(ICore &core, const ServiceRegister &serviceRegister);

  private:
    // Загрузка выбора по старту сессии (async-select + serial-guard) -> память +
    // applySpawn.
    void loadChoice(IPlayer &player, const PlayerSessionService::Session &session);

    // Диалог выбора (по команде /setspawn).
    void showDialog(IPlayer &player);

    // Применить выбор: резолв в SpawnPoint -> setSpawn. Единственный писатель
    // точки спавна; зовётся для ВСЕХ (логин/смена выбора/смена членства). Резолв
    // ВСЕГДА даёт валидную точку (пропавший дом/выход из орга -> фолбэк на вокзал).
    void applySpawn(IPlayer &player);
    SpawnPoint resolveSpawn(IPlayer &player) const;

    SpawnChoiceService &m_choiceService;
    PlayerSpawnService &m_spawnService;
    FactionService &m_factionService;
    HouseService &m_houseService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
};
