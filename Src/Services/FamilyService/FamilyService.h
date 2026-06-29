#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FamilySystem;

// Семьи — player-created социальные группы поверх фракций (не Core, не часть
// вертикали организаций). У семьи есть НАЗВАНИЕ, владелец-старейшина и список
// членов. Источник правды о семьях и членстве — здесь и в БД: семьи и все их
// члены грузятся ЦЕЛИКОМ на старте сервера и держатся в памяти (m_families),
// изменения уходят в БД сразу (write-through, как членство фракций). Серверная
// правда о семье живёт даже когда члены оффлайн.
//
// Одна семья на игрока. Вступление — только по приглашению владельца (диалог
// подтверждения у приглашённого). Создание бесплатное. Лимит членов MAX_MEMBERS.
//
// Уход ВЛАДЕЛЬЦА: владение передаётся старейшему по joined_at среди оставшихся;
// если владелец был последним членом — семья распускается. Отдельный роспуск
// (disbandFamily) — немедленное удаление семьи владельцем.
//
// id семьи генерирует сервер (единственный писатель): m_nextId = max(id)+1 на
// загрузке, INSERT с явным id — без async round-trip.
//
// Текст в памяти и БД — utf-8 (ввод из диалогов конвертируй cp1251Toutf8 до
// передачи сюда; отображение — utf8Tocp1251). Название уходит тегом в /f чат и
// в client message, поэтому санитизируется от инъекции цветокодов клиента
// ('{','}','~' обезвреживаются) и управляющих символов — см. sanitizeName.
class FamilyService final : public IService
{
    friend FamilySystem;

  public:
    FamilyService();

    using AccountId = PlayerSessionService::AccountId;

    static constexpr int NO_FAMILY = -1;
    static constexpr std::size_t MAX_MEMBERS = 20;
    static constexpr std::size_t NAME_MIN = 3;  // символов utf-8 после санитайза
    static constexpr std::size_t NAME_MAX = 24; // тег [{название}] в /f не должен раздувать строку

    struct Mem
    {
        AccountId accountId = PlayerSessionService::NO_ACCOUNT;
        std::string name;       // utf-8, ник на момент вступления (для состава семьи)
        long long joinedAt = 0; // unix-время вступления; старейший наследует владение
    };

    struct Family
    {
        int id = NO_FAMILY;
        std::string name; // utf-8
        AccountId ownerAccountId = PlayerSessionService::NO_ACCOUNT;
        long long createdAt = 0;
        std::vector<Mem> members; // отсортированы по joinedAt по возрастанию (старейший — первый)
    };

    // Итог операции членства: код для сообщения вызывающему (тексты — в системе).
    enum class Result
    {
        Ok,
        InvalidName,    // не прошла валидация длины/чистки
        NameTaken,      // имя занято (ASCII-регистронезависимо; БД-UNIQUE в utf8mb4_bin совпадает)
        AlreadyInFamily, // уже состоит в семье
        FamilyFull,     // достигнут лимит членов
        NotFound,       // семьи нет
        NoSession,      // у игрока нет активной сессии (некуда писать)
        NotLoaded,      // семьи ещё не загружены (или загрузка сорвалась) — create/join недоступны
    };

    // --- запрос состояния (игроки онлайн / резолв на старте сессии) ---
    int getFamilyId(int playerId) const;             // NO_FAMILY — не в семье / невалидный id
    const Family *getFamily(int familyId) const;     // nullptr — нет такой семьи
    bool isOwner(int playerId) const;                // владелец своей семьи (онлайн)
    // id семьи по аккаунту (в т.ч. оффлайн): NO_FAMILY — аккаунт не в семье.
    int familyByAccount(AccountId accountId) const;

    // --- операции (write-through в БД); требуют активной сессии у игрока ---
    // Создать семью с владельцем-членом player. name — utf-8 (НЕ санитизированное:
    // чистку и валидацию делает сервис).
    Result createFamily(IPlayer &player, AccountId accountId, std::string_view rawName);
    // Вступить в семью familyId (по приглашению). Перепроверяет cap и что игрок
    // ещё не в семье — клиенту/старому диалогу не доверяем.
    Result joinFamily(IPlayer &player, AccountId accountId, int familyId);
    // Выход. Владелец -> передача владения старейшему из оставшихся (или роспуск,
    // если он последний); обычный член -> просто исключение. Возвращает true, если
    // после выхода семья распущена (последний член ушёл).
    bool leaveFamily(IPlayer &player, AccountId accountId);
    // Немедленный роспуск семьи владельцем: удаление family + всех family_member,
    // очистка онлайн-слотов всех членов. false — игрок не владелец.
    bool disbandFamily(int ownerPlayerId);

    // Чистка клиентского ввода названия: цветокоды клиента ('{','}','~')
    // обезвреживаются, управляющие символы выбрасываются, края обрезаются, длина —
    // по границе utf-8 (NAME_MAX). Возвращает utf-8. Пустое/короткое —
    // отбраковывается на месте создания (validateName).
    static std::string sanitizeName(std::string_view rawUtf8);

  private:
    // --- вызывается FamilySystem ---
    // Загрузка строки family из БД на старте (до loadMember). createdAt/owner — из БД.
    void loadFamily(int id, std::string name, AccountId ownerAccountId, long long createdAt);
    // Загрузка строки family_member из БД (строго после всех loadFamily). Член
    // несуществующей семьи отбрасывается с warning.
    void loadMember(int familyId, AccountId accountId, std::string name, long long joinedAt);
    // Финализация загрузки: сортировка членов по joinedAt, выставление m_nextId,
    // снятие семей без членов (битые строки) и резолв m_accountFamily.
    void finalizeLoad();

    // Старт/конец сессии (online-слот членства). onSessionStart ставит
    // m_playerFamily из m_accountFamily; onSessionEnd чистит слот.
    void onSessionStart(int playerId, AccountId accountId);
    void onSessionEnd(int playerId);

    Family *findFamily(int familyId);
    // Уникальность имени по всем семьям, регистронезависимо (utf-8 ASCII-lower
    // достаточно: кириллица сравнивается побайтно — точное совпадение и так ловит).
    bool nameTaken(std::string_view utf8Name, int exceptId) const;
    bool validateName(std::string_view utf8Name) const; // длина NAME_MIN..NAME_MAX
    // Удалить семью целиком (память + онлайн-слоты + write-through). Внутренняя:
    // вызывается из leaveFamily (последний член) и disbandFamily.
    void destroyFamily(int familyId);
    // Сбросить онлайн-слот у всех игроков, чей m_playerFamily == familyId.
    void clearOnlineMembers(int familyId);
    // Пересчитать m_accountFamily для семьи (после изменения состава).
    void reindexAccounts();

    std::unordered_map<int, Family> m_families;
    int m_nextId = 1; // сервер — единственный писатель id семей
    // Полная загрузка семей+членов завершена. До неё (и при сбое загрузки)
    // create/join ОТКАЗЫВАЮТ: m_nextId ещё не выставлен по max(id) из БД, новый id
    // конфликтовал бы с незагруженными строками (рассинхрон память<->БД). Ставится
    // true ТОЛЬКО в finalizeLoad (success-колбэк); error-путь оставляет false.
    bool m_loaded = false;

    // Онлайн-слот: familyId игрока в сети (NO_FAMILY — не в семье / слот свободен).
    std::array<int, MAX_PLAYERS> m_playerFamily;
    // Аккаунт игрока в сети (для резолва владельца по playerId без сессии);
    // NO_ACCOUNT — слот свободен. Заполняется на старте сессии.
    std::array<AccountId, MAX_PLAYERS> m_playerAccount;
    // Резолв членства по аккаунту (для onSessionStart и оффлайн-проверок).
    std::unordered_map<AccountId, int> m_accountFamily;
};
