#include "Systems/WeaponProficiencySystem/WeaponProficiencySystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "Services/Core/PlayerCommandService/PlayerCommandService.h"
#include "Utils/Encoding/Encoding.h"
#include <array>
#include <cstdint>
#include <fmt/format.h>
#include <mysqlx/xdevapi.h>
#include <utility>
#include <vector>

namespace
{
// cp1251 для вывода игроку (как в остальных системах — локальный хелпер).
std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

// Учитываемые оружия в порядке колонок персиста. Источник правды о составе пятёрки
// — константы WeaponProficiencyService; здесь только перечисление для итерации
// (загрузка/сохранение по каждому). При изменении набора править ОБА места.
constexpr std::array<std::uint8_t, WeaponProficiencyService::WEAPON_COUNT> TRACKED_WEAPONS = {
    WeaponProficiencyService::WEAPON_DEAGLE, WeaponProficiencyService::WEAPON_SHOTGUN,
    WeaponProficiencyService::WEAPON_AK47, WeaponProficiencyService::WEAPON_M4,
    WeaponProficiencyService::WEAPON_SNIPER};
} // namespace

WeaponProficiencySystem::WeaponProficiencySystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_proficiencyService(serviceRegister.getService<WeaponProficiencyService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_weaponSkillService(serviceRegister.getService<WeaponSkillService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>())
{
    core.getPlayers().getPlayerConnectDispatcher().addEventHandler(this);

    m_sessionService.subscribeStart(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            loadProficiency(player, session);
        });
    m_sessionService.subscribeEnd(
        [this](IPlayer &player, const PlayerSessionService::Session &session)
        {
            persistProficiency(player, session);
        });

    // Level-up владения (+1, раз в SHOTS_PER_SKILL[weapon] валидных выстрелов) → поднимаем
    // соответствующий нативный weapon skill. Кросс-сервисная связка живёт здесь, в
    // системе: сервис владения остаётся без зависимости от WeaponSkillService.
    m_proficiencyService.subscribeLevelUp(
        [this](int playerId, std::uint8_t weaponId, int /*newSkill*/)
        {
            applyNativeSkill(playerId, weaponId);
        });

    // /skills — игрок смотрит своё владение по 5 учтённым оружиям (и способ
    // проверить, что прогрессия растёт от стрельбы).
    auto &commands = serviceRegister.getService<PlayerCommandService>();
    commands.add("skills", {},
                 [this](IPlayer &player, const PlayerCommandService::CommandArgs &)
                 {
                     showSkills(player);
                 },
                 {}, "навыки владения оружием", PlayerCommandService::HelpCategory::Misc);
}

void WeaponProficiencySystem::onPlayerConnect(IPlayer &player)
{
    m_proficiencyService.resetPlayer(player.getID());
    m_loaded[player.getID()] = false; // загрузка ещё не выполнялась
}

void WeaponProficiencySystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    m_proficiencyService.resetPlayer(player.getID());
    m_loaded[player.getID()] = false;
}

void WeaponProficiencySystem::loadProficiency(IPlayer &player, const PlayerSessionService::Session &session)
{
    // Запрос И вычитка строк — на воркере; на главный поток приходит владеющий
    // вектор пар (weapon, skill). Отсутствующая строка = скилл 0 (resetPlayer на
    // коннекте уже обнулил слот), её просто нет в выборке.
    DatabaseManager::selectQuery<std::vector<std::pair<int, int>>>(
        [accountId = session.accountId](mysqlx::Schema schema)
        {
            mysqlx::RowResult rows = schema.getTable("player_weapon_skill")
                                         .select("weapon", "skill")
                                         .where("account_id = :account")
                                         .bind("account", accountId)
                                         .execute();
            std::vector<std::pair<int, int>> result;
            // weapon/skill читаем как int64 и сужаем: значение вне диапазона int
            // не должно ронять get<int>() (битый ряд иначе оборвал бы всю загрузку).
            // Скилл клампится в setSkill, оружие вне 5 — игнорируется.
            while (mysqlx::Row row = rows.fetchOne())
                result.emplace_back(static_cast<int>(row.get(0).get<std::int64_t>()),
                                    static_cast<int>(row.get(1).get<std::int64_t>()));
            return result;
        },
        [this, playerId = player.getID(), serial = session.serial](std::vector<std::pair<int, int>> rows)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
                return;
            if (!m_core.getPlayers().get(playerId))
                return;

            // setSkill валидирует оружие (не из 5 — игнор) и КЛАМПИТ скилл 0..100:
            // мусор/устаревший кап из БД не осядет.
            for (const auto &[weapon, skill] : rows)
                m_proficiencyService.setSkill(playerId, static_cast<std::uint8_t>(weapon), skill);

            // Проецируем загруженное владение на нативный weapon skill по всем 5
            // оружиям (даже те, по которым строки не было — там владение 0 → уровень
            // 0, перезапишет возможный дев-оверрайд /wskill этих категорий). С этого
            // момента источник правды для 5 учтённых категорий — владение.
            for (const std::uint8_t weapon : TRACKED_WEAPONS)
                applyNativeSkill(playerId, weapon);

            // Загрузка дошла (даже если строк не было — это «0 скиллов», валидно).
            // Теперь конец сессии вправе персистить, не рискуя затереть БД нулями.
            m_loaded[playerId] = true;
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "WeaponProficiencySystem: failed to load proficiency: " + error);
        });
}

void WeaponProficiencySystem::persistProficiency(IPlayer &player, const PlayerSessionService::Session &session)
{
    // Снимок текущих скиллов на главном потоке (источник правды — сервис), затем
    // одна задача-воркер пишет UPSERT по каждому оружию. Сессия ещё активна,
    // accountId валиден. Остаток-счётчик (<5) не персистим — потеря недобора на
    // логауте допустима (так договорено).
    const int playerId = player.getID();
    // Не персистим, если загрузка скиллов не завершилась успешно: нулевой слот
    // затёр бы реальный прогресс в БД (сбой загрузки/дисконнект до её колбэка).
    if (!m_loaded[playerId])
        return;

    std::array<std::pair<std::uint8_t, int>, WeaponProficiencyService::WEAPON_COUNT> snapshot;
    for (std::size_t i = 0; i < TRACKED_WEAPONS.size(); ++i)
        snapshot[i] = {TRACKED_WEAPONS[i], m_proficiencyService.getSkill(playerId, TRACKED_WEAPONS[i])};

    DatabaseManager::throwQuery(
        [accountId = session.accountId, snapshot](mysqlx::Schema schema)
        {
            // UPSERT (как BankService): первый заход создаёт строку, последующие
            // ПЕРЕЗАПИСЫВАЮТ скилл (не суммируют — это абсолютное значение). weapon
            // хранится как серверный id оружия (PRIMARY KEY account_id+weapon).
            // Через .insert()/.update() X DevAPI это в один заход не выразить
            // (insert упал бы на дубликате ключа на втором логине), поэтому raw SQL.
            for (const auto &[weapon, skill] : snapshot)
            {
                schema.getSession()
                    .sql("INSERT INTO player_weapon_skill (account_id, weapon, skill) VALUES (?, ?, ?) "
                         "ON DUPLICATE KEY UPDATE skill = VALUES(skill)")
                    .bind(accountId, static_cast<int>(weapon), skill)
                    .execute();
            }
        },
        [](const std::string &error)
        {
            LogManager::log(Error, "WeaponProficiencySystem: failed to persist proficiency: " + error);
        });
}

PlayerWeaponSkill WeaponProficiencySystem::nativeSkillFor(std::uint8_t weaponId)
{
    // Маппинг 5 учтённых оружий → нативная категория навыка. Имена енума сверены
    // с ThirdParty/open.mp-sdk/include/player.hpp (PlayerWeaponSkill_*).
    switch (weaponId)
    {
    case WeaponProficiencyService::WEAPON_DEAGLE:
        return PlayerWeaponSkill_DesertEagle;
    case WeaponProficiencyService::WEAPON_SHOTGUN:
        return PlayerWeaponSkill_Shotgun;
    case WeaponProficiencyService::WEAPON_AK47:
        return PlayerWeaponSkill_AK47;
    case WeaponProficiencyService::WEAPON_M4:
        return PlayerWeaponSkill_M4;
    case WeaponProficiencyService::WEAPON_SNIPER:
        return PlayerWeaponSkill_Sniper;
    default:
        return PlayerWeaponSkill_Invalid; // не из 5 учтённых — применять нельзя
    }
}

int WeaponProficiencySystem::toNativeLevel(int proficiency)
{
    // Линейно владение 0..MAX_SKILL → нативный уровень 0..MAX_SKILL_LEVEL
    // (100→999, 50→499, 0→0). proficiency сервисом уже клампится в 0..MAX_SKILL;
    // считаем в int (100*999 = 99900 — без переполнения). setLevel доклампит сам.
    return proficiency * MAX_SKILL_LEVEL / WeaponProficiencyService::MAX_SKILL;
}

void WeaponProficiencySystem::applyNativeSkill(int playerId, std::uint8_t weaponId)
{
    const PlayerWeaponSkill skill = nativeSkillFor(weaponId);
    if (skill == PlayerWeaponSkill_Invalid)
        return; // оружие вне 5 учтённых — нативной категории нет

    // Игрок мог выйти (level-up приходит синхронно, но applyNativeSkill зовётся и
    // из async-колбэка загрузки — резолвим заново). Offline → тихий no-op.
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
        return;

    // getSkill валидирует playerId/оружие и вернёт 0 на промахе — уровень будет 0.
    const int proficiency = m_proficiencyService.getSkill(playerId, weaponId);
    m_weaponSkillService.setLevel(*player, skill, toNativeLevel(proficiency));
}

void WeaponProficiencySystem::showSkills(IPlayer &player)
{
    const int playerId = player.getID();

    struct Entry
    {
        const char *name; // utf-8 (конвертируется u() при выводе)
        std::uint8_t weapon;
    };
    static constexpr Entry entries[] = {
        {"Desert Eagle", WeaponProficiencyService::WEAPON_DEAGLE},
        {"Shotgun", WeaponProficiencyService::WEAPON_SHOTGUN},
        {"AK-47", WeaponProficiencyService::WEAPON_AK47},
        {"M4", WeaponProficiencyService::WEAPON_M4},
        {"Sniper Rifle", WeaponProficiencyService::WEAPON_SNIPER},
    };

    // TABLIST_HEADERS: колонки «Оружие | Владение». Первая строка — шапка колонок,
    // далее по строке на оружие, последней — жёлтая подсказка (2-я колонка пустая).
    std::string body = "Оружие\tВладение\n";
    for (const Entry &e : entries)
        body += fmt::format("{}\t{}/{}\n", e.name, m_proficiencyService.getSkill(playerId, e.weapon),
                            WeaponProficiencyService::MAX_SKILL);
    body += "{FFB400}Стреляйте из этого оружия, чтобы повысить владение\t";

    Dialog dialog;
    dialog.style = DialogStyle_TABLIST_HEADERS;
    dialog.title = u("Владение оружием");
    dialog.body = u(body);
    dialog.leftButton = u("Закрыть");
    dialog.rightButton = u(""); // одна кнопка — диалог только для просмотра
    // Просмотр: реакции на ответ нет (любая кнопка просто закрывает).
    m_dialogService.show(player, dialog, [](DialogResponse, int, StringView) {});
}
