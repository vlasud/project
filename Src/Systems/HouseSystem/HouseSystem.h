#pragma once

#include "Macro.h"
#include "Services/Core/MapIconService/MapIconService.h"
#include "Services/Core/PickupService/PickupService.h"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/TextLabelService/TextLabelService.h"
#include "Services/HouseService/HouseService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
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
//  * пикап ВХОДА (позиция создателя, vw 0): ничейный дом -> диалог занятия;
//    занятый -> телепорт в интерьер дома (контроль доступа/замки — будущее);
//  * пикап ВЫХОДА (внутри интерьера, vw дома) -> телепорт на точку выхода (vw 0);
//  * иконка на карте у входа: ничейный -> зелёная (31), занятый -> красная (32);
//  * 3D-текст у входа (номер дома + тип интерьера) — игроцкий ориентир, виден всем.
//
// ВЛАДЕНИЕ через ЗАНЯТИЕ: наступив на пикап ничейного дома, игрок подтверждает
// занятие в MSGBOX и становится владельцем (бесплатно, один дом на игрока).
// Владелец = АККАУНТ — переживает рестарт. Источники правды РАЗНЫЕ: ОПИСАНИЕ дома
// (контент, ставит дев) — houses.json; ВЛАДЕНИЕ (house -> аккаунт) — БД (таблица
// house_owner, write-through как членство фракций). Занятие пишет house_owner (не
// файл) ОДНОЙ транзакцией DELETE+INSERT (атомарно, с rollback при сбое); при ошибке
// записи оптимистичная память владения откатывается. Удаление дома стирает строку
// владения; на старте владение грузится из БД ПОСЛЕ разбора описаний (безусловно,
// на всех ветках) и применяется к живым домам (зелёная -> красная). До прихода
// зеркала занятие домов отложено флагом m_ownershipLoaded (старт-гонка «один дом на
// игрока»). Серверная правда: accountId берётся из PlayerSessionService, не от
// клиента; согласие перепроверяет, что дом жив и ещё ничейный, а игрок ещё не владеет
// домом (in-memory ownsHouse + БД UNIQUE account_id — бэкстоп).
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
    // Пересоздать радар-иконку дома по актуальному owner (зелёная/красная). У
    // глобальных иконок нет update — только remove+add; хэндл в Runtime::mapIcon
    // обновляется (нет утечки/двойного remove). Зовётся после смены владельца.
    void refreshHouseIcon(int houseId);

    // --- обработчики пикапов (живого игрока берут заново) ---
    void onEntrancePickup(int houseId, IPlayer &player);
    void onExitPickup(int houseId, IPlayer &player);

    // MSGBOX занятия ничейного дома. Согласие требует загруженного владения
    // (m_ownershipLoaded — иначе отказ «дома ещё загружаются»), перепроверяет (дом
    // жив и ещё ничейный, игрок ещё не владеет домом), выставляет владельца в памяти
    // оптимистично, пишет владение в БД одной транзакцией DELETE+INSERT (house_owner
    // write-through) и перекрашивает иконку. При сбое записи память владения
    // откатывается в errorCallback. Живого игрока и accountId берёт заново на согласии.
    void showClaimConfirm(IPlayer &player, int houseId);

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
    void loadFromFileAsync();           // ОПИСАНИЕ домов на старте; пусто при отсутствии файла
    void saveToFileAsync();             // весь набор ОПИСАНИЙ после create/remove
    // Владение из БД (house_owner) — один selectQuery на старте, чейнится ПОСЛЕ
    // разбора описаний домов на ВСЕХ ветках (пусто/битый/успех; getHouse-гард в
    // колбэке отбросит осиротевшие записи). Колбэк на главном потоке: setOwner в
    // памяти + refreshHouseIcon по живым домам. Без записи в БД — загрузка зеркала.
    // По завершении (успех ИЛИ ошибка БД) выставляет m_ownershipLoaded.
    void loadOwnershipAsync();
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
    PlayerSessionService &m_sessionService; // ключ владельца = std::to_string(accountId)

    std::unordered_map<int, Runtime> m_runtime;            // houseId -> хэндлы
    // Владение из БД легло в память (loadOwnershipAsync завершился — успехом или
    // ошибкой БД). До этого занятие домов отложено: ownsHouse() ещё не видит
    // зеркало, и можно было бы занять второй дом (старт-гонка «один дом на игрока»).
    bool m_ownershipLoaded = false;
    std::array<TimePoint, MAX_PLAYERS> m_exitGraceFrom{};     // момент последнего входа (грейс выхода)
    std::array<TimePoint, MAX_PLAYERS> m_entranceGraceFrom{}; // момент последнего выхода (грейс входа)
};
