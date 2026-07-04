#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/PlayerSessionService/PlayerSessionService.h"
#include "player.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FamilySystem;

// Семьи — player-created социальные группы поверх фракций (не Core, не часть
// вертикали организаций). У семьи есть НАЗВАНИЕ, владелец-лидер и список
// членов. Источник правды о семьях и членстве — здесь и в БД: семьи и все их
// члены грузятся ЦЕЛИКОМ на старте сервера и держатся в памяти (m_families),
// изменения уходят в БД сразу (write-through, как членство фракций). Серверная
// правда о семье живёт даже когда члены оффлайн.
//
// Одна семья на игрока. Вступление — только по приглашению владельца (диалог
// подтверждения у приглашённого). Создание бесплатное. Лимит членов MAX_MEMBERS.
//
// Уход ВЛАДЕЛЬЦА из UI недостижим: «Покинуть семью» для владельца — это ПОЛНЫЙ
// РОСПУСК (disbandFamily), а не передача власти. leaveFamily остаётся простым
// исключением РЯДОВОГО члена (наследование владения убрано вместе с той веткой
// UI — см. Docs/Family.md); роспуск последнего члена внутри leaveFamily сохранён
// как страховка на случай прямого вызова не через владельца. Отдельный роспуск
// (disbandFamily) — немедленное удаление семьи владельцем. kickMember — владелец
// исключает произвольного (в т.ч. оффлайн) члена по accountId.
//
// id семьи генерирует сервер (единственный писатель): m_nextId = max(id)+1 на
// загрузке, INSERT с явным id — без async round-trip.
//
// Текст в памяти и БД — utf-8 (ввод из диалогов конвертируй cp1251Toutf8 до
// передачи сюда; отображение — utf8Tocp1251). НАЗВАНИЕ — одно слово ЛАТИНИЦЕЙ
// (только A-Z/a-z, validateName), поэтому инъекция цветокодов в него невозможна
// по построению; sanitizeName остаётся первым шагом конвейера (чистит края/
// управляющие до валидации, мусор честно отбраковывается как InvalidName).
class FamilyService final : public IService
{
    friend FamilySystem;

  public:
    FamilyService();

    using AccountId = PlayerSessionService::AccountId;

    static constexpr int NO_FAMILY = -1;
    static constexpr std::size_t MAX_MEMBERS = 20;
    // Название — ОДНО СЛОВО ЛАТИНИЦЕЙ (без цифр, пробелов, символов, кириллицы).
    // Уникально БЕЗ учёта регистра («vlasud» занял — «Vlasud»/«vlaSuD» не
    // пройдут): в памяти equalsIgnoreCaseAscii, в БД коллация utf8mb4_general_ci.
    static constexpr std::size_t NAME_MIN = 3;  // латинских букв
    static constexpr std::size_t NAME_MAX = 24; // тег [{название}] в /f не должен раздувать строку

    struct Mem
    {
        AccountId accountId = PlayerSessionService::NO_ACCOUNT;
        std::string name;       // utf-8, ник на момент вступления (для состава семьи)
        long long joinedAt = 0; // unix-время вступления (порядок отображения в UI)
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
        NotOwner,       // операция доступна только владельцу семьи
        TargetNotFound, // цель kick не найдена среди членов семьи
        CannotKickSelf, // попытка выгнать самого себя (владельца) через kickMember
    };

    // --- запрос состояния (игроки онлайн / резолв на старте сессии) ---
    int getFamilyId(int playerId) const;             // NO_FAMILY — не в семье / невалидный id
    const Family *getFamily(int familyId) const;     // nullptr — нет такой семьи
    bool isOwner(int playerId) const;                // владелец своей семьи (онлайн)
    // id семьи по аккаунту (в т.ч. оффлайн): NO_FAMILY — аккаунт не в семье.
    int familyByAccount(AccountId accountId) const;

    // Завершена ли стартовая загрузка семей+членов (m_loaded после finalizeLoad).
    bool isLoaded() const
    {
        return m_loaded;
    }
    // Подписка на ЗАВЕРШЕНИЕ стартовой загрузки семей (одноразовое событие). Если
    // семьи уже загружены к моменту подписки — колбэк вызывается СРАЗУ (поздний
    // подписчик не пропустит one-shot). ParkedVehicleSystem подписывается, чтобы
    // грузить parked_vehicle СТРОГО после семей: обе загрузки async, порядком
    // регистрации систем гарантию дать нельзя (детерминированный порядок — только так).
    using LoadedObserver = std::function<void()>;
    void subscribeLoaded(LoadedObserver observer);

    // --- операции (write-through в БД); требуют активной сессии у игрока ---
    // Создать семью с владельцем-членом player. name — utf-8 (НЕ санитизированное:
    // чистку и валидацию делает сервис).
    Result createFamily(IPlayer &player, AccountId accountId, std::string_view rawName);
    // Вступить в семью familyId (по приглашению). Перепроверяет cap и что игрок
    // ещё не в семье — клиенту/старому диалогу не доверяем.
    Result joinFamily(IPlayer &player, AccountId accountId, int familyId);
    // Выход РЯДОВОГО члена: исключение из состава. Владелец через UI сюда не
    // попадает («Покинуть семью» для него = disbandFamily) — если всё же вызван для
    // владельца (последний член), семья распускается как страховочный путь; для
    // владельца с оставшимися членами наследование НЕ выполняется (ветка убрана
    // вместе с UI — см. Docs/Family.md). Возвращает true, если семья была распущена.
    bool leaveFamily(IPlayer &player, AccountId accountId);
    // Немедленный роспуск семьи владельцем: удаление family + всех family_member,
    // очистка онлайн-слотов всех членов. false — игрок не владелец.
    bool disbandFamily(int ownerPlayerId);
    // Исключение члена семьи владельцем. targetAccountId ищется СРЕДИ ЧЛЕНОВ (в т.ч.
    // оффлайн) семьи ownerPlayerId. Отказы: NotOwner (не владелец), NotFound (нет
    // своей семьи), TargetNotFound (аккаунт не среди членов), CannotKickSelf
    // (targetAccountId == владелец). При успехе чистит онлайн-слот цели немедленно
    // (если она сейчас в сети) — членство/чат/гейты машин семьи закрываются сразу.
    Result kickMember(int ownerPlayerId, AccountId targetAccountId);

    // Чистка клиентского ввода названия ПЕРЕД валидацией: управляющие символы
    // выбрасываются, края (пробелы) обрезаются — дружелюбно к « Corleone ».
    // Возвращает utf-8. Всё вне латиницы затем отбраковывает validateName
    // (обезвреженные цветокоды '('/')'/'-' — тоже не латиница, честный отказ).
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
    bool validateName(std::string_view utf8Name) const; // одно слово латиницей, NAME_MIN..NAME_MAX букв
    // Удалить семью целиком (память + онлайн-слоты + write-through). Внутренняя:
    // вызывается из leaveFamily (последний член) и disbandFamily.
    void destroyFamily(int familyId);
    // Сбросить онлайн-слот у всех игроков, чей m_playerFamily == familyId.
    void clearOnlineMembers(int familyId);
    // Сбросить онлайн-слот ОДНОГО аккаунта (kick цели, если она сейчас в сети).
    void clearOnlineMember(AccountId accountId);
    // Пересчитать m_accountFamily для семьи (после изменения состава).
    void reindexAccounts();

    std::unordered_map<int, Family> m_families;
    int m_nextId = 1; // сервер — единственный писатель id семей
    // Полная загрузка семей+членов завершена. До неё (и при сбое загрузки)
    // create/join ОТКАЗЫВАЮТ: m_nextId ещё не выставлен по max(id) из БД, новый id
    // конфликтовал бы с незагруженными строками (рассинхрон память<->БД). Ставится
    // true ТОЛЬКО в finalizeLoad (success-колбэк); error-путь оставляет false.
    bool m_loaded = false;

    // One-shot подписчики на завершение стартовой загрузки семей.
    std::vector<LoadedObserver> m_loadedObservers;

    // Онлайн-слот: familyId игрока в сети (NO_FAMILY — не в семье / слот свободен).
    std::array<int, MAX_PLAYERS> m_playerFamily;
    // Аккаунт игрока в сети (для резолва владельца по playerId без сессии);
    // NO_ACCOUNT — слот свободен. Заполняется на старте сессии.
    std::array<AccountId, MAX_PLAYERS> m_playerAccount;
    // Резолв членства по аккаунту (для onSessionStart и оффлайн-проверок).
    std::unordered_map<AccountId, int> m_accountFamily;
};
