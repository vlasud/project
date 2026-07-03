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
// house_owner, write-through как членство фракций). До прихода зеркала занятие
// домов отложено флагом HouseService::isOwnershipLoaded (старт-гонка «один дом на
// игрока»). Серверная правда: accountId берётся из PlayerSessionService, не от
// клиента; согласие перепроверяет, что дом жив и ещё ничейный, а игрок ещё не владеет
// домом (in-memory ownsHouse + БД UNIQUE account_id — бэкстоп).
//
// ПЕРСИСТ ВЛАДЕНИЯ — ЕДИНАЯ ТОЧКА (onOwnerChanged, подписка на
// HouseService::subscribeOwnerChanged): И занятие (HouseSystem::showClaimConfirm),
// И передача/выселение (HomeMenuSystem через HouseService::setOwner) идут через
// один и тот же наблюдатель — пишут house_owner ОДНОЙ транзакцией DELETE+INSERT
// (newKey непустой) либо DELETE (newKey пустой, выселение) и перекрашивают иконку.
// При сбое записи оптимистичная память откатывается на oldKey. Так персист не
// дублируется в нескольких системах — HomeMenuSystem лишь меняет память через
// setOwner, БД/иконку приводит этот наблюдатель. Удаление дома стирает строку
// владения отдельно (removeHouse не идёт через setOwner); на старте владение
// грузится из БД ПОСЛЕ разбора описаний (безусловно, на всех ветках) и
// применяется к живым домам (зелёная -> красная).
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
    // (HouseService::isOwnershipLoaded — иначе отказ «дома ещё загружаются»), перепроверяет (дом
    // жив и ещё ничейный, игрок ещё не владеет домом), выставляет владельца в памяти
    // оптимистично (setOwner) — персист/иконку доводит onOwnerChanged. Живого
    // игрока и accountId берёт заново на согласии.
    void showClaimConfirm(IPlayer &player, int houseId);

    // ЕДИНАЯ точка персиста владения (подписка на HouseService::subscribeOwnerChanged):
    // зовётся ПОСЛЕ любого успешного setOwner (занятие/передача/выселение). Пишет
    // house_owner ОДНОЙ транзакцией: newKey непустой -> DELETE по house_id/account_id
    // + INSERT (та же атомарность, что была у занятия); newKey пустой -> DELETE по
    // house_id (выселение). При сбое БД откатывает память на oldKey и перекрашивает
    // иконку обратно (дом мог быть удалён/перезанят за время запроса — гард по
    // getHouse/owner). Всегда перекрашивает иконку на успешной попытке записи
    // (оптимистично, до ответа БД — как было при занятии).
    void onOwnerChanged(int houseId, const std::string &oldKey, const std::string &newKey);

    // --- дев-меню (/house) ---
    void showMain(IPlayer &player);
    void showCreatePicker(IPlayer &player);       // LIST каталога интерьеров
    void showCapInput(IPlayer &player, int interiorIndex); // INPUT лимита парковки после выбора интерьера
    void showHouseList(IPlayer &player);          // LIST существующих домов
    void showPickById(IPlayer &player);           // INPUT id -> подменю того дома
    void showHouseMenu(IPlayer &player, int houseId);     // подменю: телепорт/удалить
    void showDeleteConfirm(IPlayer &player, int houseId); // MSGBOX подтверждения

    // Создать дом + persist + сообщить. parkingCap уже провалидирован вводом, но
    // createHouse клампит его повторно (двойная страховка).
    void createHouseFor(IPlayer &player, int interiorIndex, int parkingCap);
    void deleteHouse(IPlayer &player, int houseId);          // удалить дом + persist

    // --- файлы (диск — в тредпуле, колбэки на главном потоке) ---
    void loadFromFileAsync();           // ОПИСАНИЕ домов на старте; пусто при отсутствии файла
    void saveToFileAsync();             // весь набор ОПИСАНИЙ после create/remove
    // Владение из БД (house_owner) — один selectQuery на старте, чейнится ПОСЛЕ
    // разбора описаний домов на ВСЕХ ветках (пусто/битый/успех; getHouse-гард в
    // колбэке отбросит осиротевшие записи). Колбэк на главном потоке: setOwnerSilent
    // (БЕЗ нотификации onOwnerChanged — иначе загрузка зеркала спровоцировала бы
    // избыточный write-through обратно в ту же БД) + refreshHouseIcon по живым домам.
    // По завершении (успех ИЛИ ошибка БД) зовёт HouseService::markOwnershipLoaded
    // (флаг готовности + one-shot оповещение подписчиков, напр. SpawnChoiceSystem).
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
    // Готовность владения (загрузка house_owner из БД завершилась) живёт в
    // HouseService (isOwnershipLoaded + subscribeOwnershipLoaded): один источник
    // правды + one-shot оповещение подписчиков (SpawnChoiceSystem).
    std::array<TimePoint, MAX_PLAYERS> m_exitGraceFrom{};     // момент последнего входа (грейс выхода)
    std::array<TimePoint, MAX_PLAYERS> m_entranceGraceFrom{}; // момент последнего выхода (грейс входа)
};
