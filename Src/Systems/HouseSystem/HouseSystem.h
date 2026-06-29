#pragma once

#include "Macro.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/HouseService/HouseService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// Дома — player houses (бизнес-фича, НЕ Core). Команда /house (DEVELOPER_LEVEL,
// Hidden) открывает дев-меню: создать дом в текущей позиции, список домов с
// телепортом ко входу и удалением. Источник правды и JSON-персист — HouseService.
//
// Геймплей дома событийный (пикапы/иконка), per-tick работы нет:
//  * пикап ВХОДА (позиция создателя, vw 0) -> телепорт в интерьер дома;
//  * пикап ВЫХОДА (внутри интерьера, vw дома) -> телепорт на точку выхода (vw 0);
//  * иконка на карте у входа: ничейный -> зелёная, занятый -> красная (сейчас
//    все дома ничейные);
//  * 3D-текст у входа (номер дома + тип интерьера) — игроцкий ориентир, виден всем.
//
// Все перемещения — ТОЛЬКО через PlayerLocationService (иначе анти-чит откатит).
class HouseSystem : public BaseSystem
{
  public:
    HouseSystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    // Рантайм-хэндлы дома: пикапы/иконка/3D-текст. В JSON не хранятся —
    // пересоздаются на загрузке и при создании, снимаются при удалении (нет
    // утечки/двойного remove).
    struct Runtime
    {
        int entrancePickup = -1;
        int exitPickup = -1;
        int mapIcon = -1;
        int label = -1; // 3D-текст у входа (номер дома + тип интерьера)
    };

    // Завести рантайм-хэндлы для дома (пикап входа + иконка + пикап выхода + 3D-текст).
    void spawnHouse(const HouseService::House &house);
    // Снять рантайм-хэндлы дома (пикапы + иконка + 3D-текст).
    void despawnHouse(int houseId);

    // --- обработчики пикапов (живого игрока берут заново) ---
    void onEntrancePickup(int houseId, IPlayer &player);
    void onExitPickup(int houseId, IPlayer &player);

    // --- дев-меню (/house) ---
    void showMain(IPlayer &player);
    void showCreatePicker(IPlayer &player);       // LIST каталога интерьеров
    void showHouseList(IPlayer &player);          // LIST существующих домов
    void showPickById(IPlayer &player);           // INPUT id -> подменю того дома
    void showHouseMenu(IPlayer &player, int houseId);     // подменю: телепорт/удалить
    void showDeleteConfirm(IPlayer &player, int houseId); // MSGBOX подтверждения

    void createHouseFor(IPlayer &player, int interiorIndex); // создать дом + persist + сообщить
    void deleteHouse(IPlayer &player, int houseId);          // удалить дом + persist

    // --- файлы (диск — в тредпуле, колбэки на главном потоке) ---
    void loadFromFileAsync();           // на старте; пусто при отсутствии файла
    void saveToFileAsync();             // весь набор после create/remove
    // Крах-безопасный парсер фиксированной схемы домов: на любой мусор/обрезку
    // возвращает то, что удалось распознать (битые объекты молча пропускаются),
    // НИКОГДА не падает и не делает UB. На воркере выполняется только чтение файла
    // в строку (task.func); сам разбор зовётся из task.callback на ГЛАВНОМ потоке.
    // ok=false — контент не похож на наш JSON (повод переименовать файл в .bak).
    static std::vector<HouseService::House> parse(const std::string &content, bool &ok);

    // Грейс выхода: per-player момент входа в дом. Пикап выхода смещён от точки
    // спавна, но грейс — страховка от мгновенного ре-триггера на лаге позиции.
    bool inExitGrace(int playerId) const;
    // Грейс входа: per-player момент выхода из дома. Точка выхода в EXIT_DISTANCE от
    // пикапа входа может тут же сработать вход — грейс гасит ре-триггер (симметрично
    // выходному, та же длительность EXIT_GRACE).
    bool inEntranceGrace(int playerId) const;

    MapIconService &m_mapIconService;
    PickupService &m_pickupService;
    PlayerLocationService &m_locationService;
    PlayerDialogService &m_dialogService;
    TextLabelService &m_labelService;

    std::unordered_map<int, Runtime> m_runtime;            // houseId -> хэндлы
    std::array<TimePoint, MAX_PLAYERS> m_exitGraceFrom{};     // момент последнего входа (грейс выхода)
    std::array<TimePoint, MAX_PLAYERS> m_entranceGraceFrom{}; // момент последнего выхода (грейс входа)
};
