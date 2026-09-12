#pragma once

#include "Services/Core/PlayerChatService/PlayerChatService.h"
#include "Services/Core/PlayerDialogService/PlayerDialogService.h"
#include "Services/Core/ScreenNoticeService/ScreenNoticeService.h"
#include "Services/Core/VehicleService/VehicleService.h"
#include "Services/FamilyService/FamilyService.h"
#include "Services/ParkedVehicleService/ParkedVehicleService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "Systems/BaseSystem.h"
#include "player.hpp"
#include <cstdint>
#include <vector>

// Привод бизнес-фичи «Семьи»:
//  * на старте грузит ВСЕ семьи и их членов из БД в FamilyService;
//  * по старту/концу сессии резолвит/чистит онлайн-членство игрока;
//  * /family — одна команда, открывающая меню по состоянию игрока: без семьи —
//    «Создать семью»; в семье — 5 пунктов (Информация/Члены семьи/Транспорт
//    семьи/Пригласить/Покинуть семью). Исключение — из подменю члена (список
//    «Члены семьи» -> выбор -> «Исключить»), отдельного корневого пункта нет;
//  * /f — семейный чат всем онлайн-членам своей семьи.
//
// Всё членство и лидер — серверная правда из FamilyService (клиенту/старому
// диалогу не доверяем): пригласить/исключить может только лидер, согласие
// приглашённого перепроверяет состояние на момент клика.
//
// Создание семьи свободное: гейта по дому НЕТ (убран решением владельца) — любой
// авторизованный игрок без семьи создаёт её бесплатно.
//
// ПРАВИЛО ВИДИМОСТИ: все 5 пунктов меню «в семье» (и оба пункта подменю члена)
// видны ВСЕГДА всем членам — недоступность (не лидер) объясняет сам обработчик
// сообщением при клике, не скрытием пункта (как CarMenuSystem/HomeMenuSystem).
class FamilySystem : public BaseSystem
{
  public:
    FamilySystem(ICore &core, const ServiceRegister &serviceRegister);

    void initialize(IComponentList *components) override;

  private:
    void loadAll(); // загрузка семей и членов из БД на старте

    // Семейный чат /f: рассылка всем онлайн-членам своей семьи.
    void familyChat(IPlayer &player, StringView rawText);

    // Меню /family: динамический вектор действий по состоянию игрока (без семьи —
    // только создание; в семье — 5 пунктов, все видны всегда).
    void showMenu(IPlayer &player);
    void showCreateInput(IPlayer &player);

    // 1: карточка семьи (TABLIST_HEADERS «Поле | Значение»).
    void showInfo(IPlayer &player);
    // 2: состав семьи (TABLIST_HEADERS «Игрок | Статус», онлайн-сортировка).
    // Выбор строки ведёт в подменю члена; listItem резолвится по СНИМКУ порядка
    // на момент показа (онлайн-сортировка могла перетасовать список под диалогом).
    void showMembers(IPlayer &player);
    // Подменю выбранного члена: «Информация» / «Исключить» (оба видны всем —
    // правило видимости; гейты кика в обработчике).
    void showMemberActions(IPlayer &player, FamilyService::AccountId targetAccount);
    // Карточка члена (TABLIST_HEADERS «Поле | Значение», расширяемая).
    void showMemberInfo(IPlayer &player, FamilyService::AccountId targetAccount);
    // Актуальная запись состава по accountId (nullptr — уже не член); ре-валидация
    // на каждом входе подменю/карточки.
    const FamilyService::Mem *memberByAccount(int playerId, FamilyService::AccountId targetAccount) const;
    // Онлайн — актуальный «Ник[id]», оффлайн — чищенный снимок Mem::name.
    std::string memberDisplayName(FamilyService::AccountId targetAccount, const FamilyService::Mem &member) const;
    // 3: список расшаренных семье машин (формат /car «Мои машины»); выбор строки —
    // вызов машины к её месту парковки (единственный вход в вызов для члена семьи).
    void showVehicles(IPlayer &player);
    // 4: приглашение по id — только лидер.
    void showInviteInput(IPlayer &player);
    void showInviteConfirm(IPlayer &invited, int inviterId, int familyId, std::uint32_t inviterSerial);
    // Подтверждение исключения (только из подменю члена — снимок targetAccount).
    void showKickConfirm(IPlayer &owner, FamilyService::AccountId targetAccount, const std::string &targetName);
    // 5: «Покинуть семью» — для лидера это ПОЛНЫЙ РОСПУСК, для рядового — выход.
    void showLeaveConfirm(IPlayer &player);

    // Действия меню (порядок фиксирован — диспетчер по вектору, не по индексам).
    enum class Action
    {
        Create,
        Info,
        Members,
        Vehicles,
        Invite,
        Leave,
    };
    // Собрать доступные игроку действия в порядке показа.
    std::vector<Action> buildActions(int playerId) const;

    FamilyService &m_familyService;
    PlayerSessionService &m_sessionService;
    PlayerDialogService &m_dialogService;
    PlayerChatService &m_chatService;
    ParkedVehicleService &m_parkedService; // семейный парк (список + вызов машины членом семьи)
    VehicleService &m_vehicleService;      // для ParkedVehicleRow::build (топливо/водитель)
    ScreenNoticeService &m_screenNotice;   // праздничный попап создания семьи
};
