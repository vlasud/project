#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Server/Components/Dialogs/dialogs.hpp"
#include "Services/Core/PlayerMoneyService/PlayerMoneyService.h"
#include "ThreadPool/ThreadPool.h"
#include "Utils/Encoding/Encoding.h"
#include "core.hpp"
#include "mysqlx/xdevapi.h"
#include "sodium/crypto_pwhash.h"
#include "types.hpp"
#include <fmt/format.h>
#include <optional>
#include <sodium.h>
#include <string>
#include <utility>

namespace
{
// Результат логин-запроса (по имени): аккаунт + активный бан одним SELECT.
// banDaysLeft — остаток дней активного бана (CEIL), 0 — бана нет. Вычитывается
// на воркере; mysqlx-объекты границу потока не пересекают (контракт).
struct LoginRow
{
    std::int64_t accountId = 0;
    std::string passwordHash;
    int skin = 0;
    std::uint8_t sex = PlayerSessionService::SEX_MALE;
    int banDaysLeft = 0;
};
} // namespace

PlayerAuthSystem::PlayerAuthSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister), m_authService(serviceRegister.getService<PlayerAuthService>()),
      m_connectionVersionService(serviceRegister.getService<PlayerConnectionVersionService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_locationService(serviceRegister.getService<PlayerLocationService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_weaponService(serviceRegister.getService<PlayerWeaponService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_moneyPersistService(serviceRegister.getService<PlayerMoneyPersistService>()),
      m_weaponPersistService(serviceRegister.getService<PlayerWeaponPersistService>()),
      m_spawnService(serviceRegister.getService<PlayerSpawnService>()),
      m_skinService(serviceRegister.getService<PlayerSkinService>()),
      m_personalSkinService(serviceRegister.getService<PlayerPersonalSkinService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>())
{
    listen(core.getPlayers().getPlayerConnectDispatcher(), this);
    listen(core.getPlayers().getPlayerChangeDispatcher(), this);
    listen(core.getPlayers().getPlayerSpawnDispatcher(), this);

    // Late-применение персиста: если логин-спавн (applyPersistedEquipment)
    // прошёл раньше, чем async-загрузка PlayerMoneyPersistService/
    // PlayerWeaponPersistService, спавн взводит m_awaitingXxxApply — здесь
    // довершаем выдачу, как только кэш пришёл. В штатном случае (загрузка
    // раньше спавна) флаги не взведены, наблюдатель — no-op (двойной выдачи
    // нет).
    m_moneyPersistService.subscribeMoneyLoaded(
        [this](IPlayer &player, unsigned long long cash)
        {
            const int playerId = player.getID();
            if (playerId < 0 || playerId >= MAX_PLAYERS || !m_awaitingMoneyApply[playerId])
                return;
            m_awaitingMoneyApply[playerId] = false;
            m_moneyService.setMoney(player, cash);
        });
    m_weaponPersistService.subscribeWeaponsLoaded(
        [this](IPlayer &player, const std::vector<std::pair<std::uint8_t, int>> &weapons)
        {
            const int playerId = player.getID();
            if (playerId < 0 || playerId >= MAX_PLAYERS || !m_awaitingWeaponsApply[playerId])
                return;
            m_awaitingWeaponsApply[playerId] = false;
            for (const auto &[weaponId, ammo] : weapons)
                m_weaponService.giveWeapon(player, weaponId, static_cast<std::uint32_t>(ammo));
            m_weaponPersistService.markWeaponsApplied(playerId);
        });
}

void PlayerAuthSystem::onPlayerConnect(IPlayer &player)
{
    // Боты (NPC) авторизацию не проходят: аккаунта у них нет, диалог показать
    // некому, а таймеры авторизации вышибли бы их из игры.
    if (player.isBot())
    {
        return;
    }

    if (m_authService.getAuthState(player.getID()) != PlayerAuthService::EAuthState::UNKNOWN)
    {
        player.sendClientMessage(Colour::White(),
                                 Encoding::utf8Tocp1251("Что-то пошло не так... Попробуйте перезайти"));
        player.kick();
        return;
    }

    resetState(player.getID());
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::AUTHORIZING);
}

void PlayerAuthSystem::onPlayerDisconnect(IPlayer &player, PeerDisconnectReason reason)
{
    resetState(player.getID());
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::UNKNOWN);
}

void PlayerAuthSystem::onPlayerKeyStateChange(IPlayer &player, uint32_t newKeys, uint32_t oldKeys)
{
    // player.sendClientMessage(Colour::White(), std::to_string(newKeys));
    // static std::array<int, 5> skins = {1, 2, 3, 4, 5};
}

void PlayerAuthSystem::onPlayerSpawn(IPlayer &player)
{
    // Респаун после finalize (выход из спектейта): сервисы уже отработали свой
    // onSpawn (порядок регистрации систем), теперь экипировка не будет сброшена.
    // Позицию/скин/интерьер применил PlayerSpawnService (источник правды о
    // спавне) — здесь только экипировка.
    if (m_pendingSpawnSetup[player.getID()])
    {
        m_pendingSpawnSetup[player.getID()] = false;
        applyPersistedEquipment(player);
        return;
    }

    if (m_authService.getAuthState(player.getID()) != PlayerAuthService::EAuthState::AUTHORIZING)
    {
        return;
    }

    m_stateService.setSpectating(player, true);
    player.setPosition({2055.8442, -1104.7142, 24.4337});
    player.interpolateCameraPosition({2059.5425, -1104.4227, 30.5487}, {2059.5425, -1104.4227, 30.5487}, 1000,
                                     PlayerCameraCutType::PlayerCameraCutType_Move);
    player.interpolateCameraLookAt({2055.8442, -1104.7142, 30.4337}, {2055.8442, -1104.7142, 30.4337}, 1000,
                                   PlayerCameraCutType::PlayerCameraCutType_Move);

    std::string name = player.getName().to_string();
    const int requestConnectionVersion = m_connectionVersionService.getVersion(player.getID());

    DatabaseManager::selectQuery<std::optional<LoginRow>>(
        [name = std::move(name)](mysqlx::Schema schema)
        {
            // LEFT JOIN ban активной строки (banned_until > NOW()): остаток дней
            // считает БД (CEIL по секундам), бана нет → NULL → 0. Один SELECT на
            // логин (а не отдельный запрос), без гонки за активностью бана.
            mysqlx::SqlResult result =
                schema.getSession()
                    .sql("SELECT p.id, p.password_hash, p.skin, "
                         "p.sex, CEIL(TIMESTAMPDIFF(SECOND, NOW(), b.banned_until) / 86400) "
                         "FROM player p "
                         "LEFT JOIN ban b ON b.account_id = p.id AND b.banned_until > NOW() "
                         "WHERE p.name = ? LIMIT 1")
                    .bind(name)
                    .execute();
            std::optional<LoginRow> account;
            if (mysqlx::Row row = result.fetchOne())
            {
                LoginRow data;
                data.accountId = row.get(0).get<std::int64_t>();
                data.passwordHash = row.get(1).get<std::string>();
                // skin читаем как int64 и сужаем: значение вне диапазона int не
                // должно ронять get<int>() при рассинхроне схемы (иначе битый ряд =
                // вечный kick аккаунта) — мусор отфильтрует isValidSkin в finalize.
                data.skin = static_cast<int>(row.get(2).get<std::int64_t>());
                // sex — как записан при регистрации (0 муж / 1 жен); сужаем так же
                // защитно, мусор трактуется как «не мужской» только при значении != 0.
                data.sex = static_cast<std::uint8_t>(row.get(3).get<std::int64_t>());
                // banDaysLeft: NULL (нет активного бана) → 0; иначе CEIL дней.
                if (!row.get(4).isNull())
                    data.banDaysLeft = static_cast<int>(row.get(4).get<std::int64_t>());
                account = std::move(data);
            }
            return account;
        },
        [this, requestConnectionVersion, playerId = player.getID()](std::optional<LoginRow> account)
        {
            if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
            {
                return;
            }

            if (!account)
            {
                runRegistration(playerId);
                return;
            }

            // Аккаунт под активным баном — НЕ пускаем (доступ не выдаём, сессию не
            // стартуем): сообщаем остаток и кикаем. Проверка серверная, до логина.
            if (account->banDaysLeft > 0)
            {
                if (IPlayer *player = m_core.getPlayers().get(playerId))
                {
                    player->sendClientMessage(
                        Colour::White(),
                        Encoding::utf8Tocp1251(fmt::format("Аккаунт заблокирован. Осталось: {} дн.",
                                                           account->banDaysLeft)));
                    player->kick();
                }
                return;
            }

            m_loginData[playerId].accountId = account->accountId;
            m_loginData[playerId].passwordHash = std::move(account->passwordHash);
            // Личный скин из БД; валидируется при применении в finalize (фолбэк
            // на дефолт там же). Невалидное значение из БД безопасно.
            m_loginData[playerId].personalSkin = account->skin;
            m_loginData[playerId].sex = account->sex;
            runLogin(playerId);
        },
        [this, requestConnectionVersion, playerId = player.getID()](const std::string &)
        {
            // Запрос упал (БД недоступна) — не оставляем игрока висеть в спектейте.
            if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
            {
                return;
            }
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            player->sendClientMessage(Colour::White(),
                                      Encoding::utf8Tocp1251("Ошибка сервера. Попробуйте зайти позже"));
            player->kick();
        });
}

void PlayerAuthSystem::resetState(int playerId)
{
    m_loginData[playerId] = {};
    m_registrationData[playerId] = {};
    m_pendingSpawnSetup[playerId] = false;
    m_awaitingMoneyApply[playerId] = false;
    m_awaitingWeaponsApply[playerId] = false;
}

void PlayerAuthSystem::runRegistration(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showRegistrationPasswordDialog(*player);
}

void PlayerAuthSystem::runLogin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showLoginDialog(*player);
}

void PlayerAuthSystem::runChooseSex(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }
    showChooseSexDialog(*player);
}

void PlayerAuthSystem::runSelectSkin(int playerId)
{
    IPlayer *player = m_core.getPlayers().get(playerId);
    if (!player)
    {
        return;
    }

    finalizeRegistration(*player);
}

void PlayerAuthSystem::showLoginDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Авторизация");
    dialog.body = Encoding::utf8Tocp1251(fmt::format("Аккаунт: {}\n\nВведите пароль", player.getName()));
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Выйти");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 player->kick();
                                 return;
                             }

                             if (text.size() < 8)
                             {
                                 player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Неверный пароль!"));
                                 showLoginDialog(*player);
                                 return;
                             }

                             // Захват версии соединения ДО долгого argon2-verify: слот playerId
                             // может за это время достаться другому игроку (reuse после дисконнекта),
                             // и без сверки колбэк авторизовал бы его в чужой аккаунт без пароля.
                             const int requestConnectionVersion = m_connectionVersionService.getVersion(playerId);

                             ThreadPool::Task<bool> task;
                             task.func = [password = text.to_string(), hash = m_loginData[playerId].passwordHash]()
                             {
                                 return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
                             };
                             task.callback = [this, playerId, requestConnectionVersion](bool verified)
                             {
                                 if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
                                 {
                                     return;
                                 }

                                 IPlayer *player = m_core.getPlayers().get(playerId);
                                 if (!player)
                                 {
                                     return;
                                 }

                                 if (!verified)
                                 {
                                     player->sendClientMessage(Colour::White(),
                                                               Encoding::utf8Tocp1251("Неверный пароль!"));
                                     showLoginDialog(*player);
                                     return;
                                 }

                                 // Сессия — после пароля: до проверки нельзя раскрывать,
                                 // что аккаунт в сети (это уже информация о владельце).
                                 if (!m_sessionService.start(*player, m_loginData[playerId].accountId, m_loginData[playerId].sex))
                                 {
                                     player->sendClientMessage(
                                         Colour::White(), Encoding::utf8Tocp1251("Этот аккаунт уже в игре"));
                                     player->kick();
                                     return;
                                 }

                                 finalize(*player, m_loginData[playerId].personalSkin);
                             };

                             ThreadPool::addTask(std::move(task));
                         });
}

void PlayerAuthSystem::showRegistrationPasswordDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Пароль");
    dialog.body = Encoding::utf8Tocp1251("Придумайте и введите пароль. Минимум 8 символов");
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Выйти");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 player->kick();
                                 return;
                             }

                             if (text.size() < 8)
                             {
                                 player->sendClientMessage(Colour::White(),
                                                           Encoding::utf8Tocp1251("Минимум 8 символов!"));
                                 showRegistrationPasswordDialog(*player);
                                 return;
                             }

                             m_registrationData[playerId].password = text.to_string();
                             showRegistrationConfirmDialog(*player);
                         });
}

void PlayerAuthSystem::showRegistrationConfirmDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_PASSWORD;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Подтверждение пароля");
    dialog.body = Encoding::utf8Tocp1251("Повторите введенный пароль. Можете вернуться назад и изменить пароль");
    dialog.leftButton = Encoding::utf8Tocp1251("Далее");
    dialog.rightButton = Encoding::utf8Tocp1251("Назад");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView text)
                         {
                             IPlayer *player = m_core.getPlayers().get(playerId);
                             if (!player)
                             {
                                 return;
                             }

                             if (response == DialogResponse_Right)
                             {
                                 showRegistrationPasswordDialog(*player);
                                 return;
                             }

                             if (m_registrationData[playerId].password != text.to_string())
                             {
                                 player->sendClientMessage(Colour::White(),
                                                           Encoding::utf8Tocp1251("Пароли не совпадают"));
                                 showRegistrationConfirmDialog(*player);
                                 return;
                             }

                             runChooseSex(playerId);
                         });
}

void PlayerAuthSystem::showChooseSexDialog(IPlayer &player)
{
    Dialog dialog;
    dialog.style = DialogStyle_MSGBOX;
    dialog.title = Encoding::utf8Tocp1251("Регистрация - Пол");
    dialog.body = Encoding::utf8Tocp1251("Выберите пол персонажа");
    dialog.leftButton = Encoding::utf8Tocp1251("Мужской");
    dialog.rightButton = Encoding::utf8Tocp1251("Женский");

    m_dialogService.show(player, dialog,
                         [this, playerId = player.getID()](DialogResponse response, int, StringView)
                         {
                             m_registrationData[playerId].sex =
                                 (response == DialogResponse_Left) ? ESex::MALE : ESex::FEMALE;
                             runSelectSkin(playerId);
                         });
}

void PlayerAuthSystem::finalizeRegistration(IPlayer &player)
{
    std::string name = player.getName().to_string();
    std::string password = std::move(m_registrationData[player.getID()].password);
    const int requestConnectionVersion = m_connectionVersionService.getVersion(player.getID());
    // Дефолтный личный скин нового аккаунта — по выбранному полу. Пишем его в БД
    // сразу (а не полагаемся на DEFAULT колонки), чтобы женский дефолт тоже
    // персистился, и применяем тем же значением в finalize.
    const std::uint8_t sexValue = static_cast<std::uint8_t>(m_registrationData[player.getID()].sex);
    const int defaultSkin = PlayerPersonalSkinService::defaultSkinForSex(sexValue);

    // Insert и чтение id — одним заданием на воркере: сессии нужен id аккаунта,
    // fire-and-forget insert его не даёт.
    DatabaseManager::selectQuery<std::optional<std::int64_t>>(
        [name, password = std::move(password), sex = sexValue, defaultSkin](mysqlx::Schema schema)
        {
            char hash[crypto_pwhash_STRBYTES] = {0};
            crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE);

            schema.getTable("player")
                .insert("name", "password_hash", "sex", "skin")
                .values(name, hash, sex, defaultSkin)
                .execute();

            mysqlx::RowResult result =
                schema.getTable("player").select("id").where("name = :name").limit(1).bind("name", name).execute();
            std::optional<std::int64_t> accountId;
            if (mysqlx::Row row = result.fetchOne())
                accountId = row.get(0).get<std::int64_t>();
            return accountId;
        },
        [this, requestConnectionVersion, playerId = player.getID(), defaultSkin,
         sex = sexValue](std::optional<std::int64_t> accountId)
        {
            if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
            {
                return; // в слоте уже другое подключение
            }
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player)
            {
                return;
            }
            if (!accountId)
            {
                player->sendClientMessage(Colour::White(),
                                          Encoding::utf8Tocp1251("Ошибка сервера. Попробуйте зайти позже"));
                player->kick();
                return;
            }

            if (!m_sessionService.start(*player, *accountId, sex))
            {
                player->sendClientMessage(Colour::White(), Encoding::utf8Tocp1251("Этот аккаунт уже в игре"));
                player->kick();
                return;
            }
            finalize(*player, defaultSkin);
        },
        [this, requestConnectionVersion, playerId = player.getID()](const std::string &)
        {
            if (m_connectionVersionService.getVersion(playerId) != requestConnectionVersion)
            {
                return;
            }
            if (IPlayer *player = m_core.getPlayers().get(playerId))
            {
                player->sendClientMessage(Colour::White(),
                                          Encoding::utf8Tocp1251("Ошибка сервера. Попробуйте зайти позже"));
                player->kick();
            }
        });
}

void PlayerAuthSystem::finalize(IPlayer &player, int personalSkin)
{
    m_authService.setPlayerAuthenticated(player.getID(), PlayerAuthService::EAuthState::AUTHENTICATED);

    // Личный (гражданский) скин аккаунта. Валидируем (фолбэк на мужской дефолт —
    // на случай мусора/нуля из БД), кладём в PlayerPersonalSkinService (источник
    // правды о личном скине для возврата из фракции) и применяем через
    // PlayerSkinService. ВАЖНО: применяем ДО старта членства — FactionSystem на
    // sessionStart грузит членство АСИНХРОННО, его колбэк отстреливает позже и
    // захватывает текущий (уже личный) скин как «гражданский» для возврата.
    //
    // ПОРЯДОК КРИТИЧЕН: skin ставим ДО setSpawn. PlayerSpawnService::setSpawn
    // строит spawn-инфо (SetSpawnInfo RPC) из getSkin() НА МОМЕНТ вызова. Если
    // сделать setSpawn раньше — в spawn-инфо уедет ещё дефолтный скин слота, и
    // респаун из спектейта появит игрока в нём, а не в личном/органном.
    const int validSkin =
        PlayerSkinService::isValidSkin(personalSkin) ? personalSkin : PlayerPersonalSkinService::DEFAULT_SKIN_MALE;
    m_personalSkinService.setSkin(player.getID(), validSkin);
    m_skinService.setSkin(player, validSkin);

    // Точка появления после входа — через единый источник правды о спавне:
    // выход из спектейта вызовет респаун ровно в неё (и в неё же — все
    // последующие смерти, пока бизнес-логика не переустановит спавн). setSpawn
    // строит spawn-инфо уже с применённым выше скином.
    SpawnPoint spawn;
    spawn.position = {1762.1505f, -1896.2495f, 13.5621f};
    m_spawnService.setSpawn(player, spawn);

    // Оружие и деньги нельзя выдавать здесь: событие спавна придёт позже и
    // сбросит их (инвентарь чистится на спавне). Экипировка — в onPlayerSpawn.
    m_pendingSpawnSetup[player.getID()] = true;
    m_stateService.setSpectating(player, false);
}

void PlayerAuthSystem::applyPersistedEquipment(IPlayer &player)
{
    const int playerId = player.getID();

    // Раньше тут был хардкод (Deagle+M4, $1500), теперь наличные/оружие приходят
    // из PlayerMoneyPersistService/PlayerWeaponPersistService (см.
    // Docs/Persistence.md). Оружие сбрасывается на КАЖДОМ спавне — giveWeapon
    // строго здесь, не раньше: PlayerWeaponService::onSpawn уже отработал.
    //
    // НАЛИЧНЫЕ ЗДЕСЬ НЕ ВОССТАНАВЛИВАЮТСЯ ИЗ КЭША. cachedMoney — снимок на момент
    // логина, он больше никогда не обновляется, а момент спавна задаёт КЛИЕНТ
    // (RPC Spawn). Абсолютный setMoney(cachedMoney) откатывал бы всё, что случилось
    // с балансом между логином и спавном: аукционный возврат, /pay, выплату
    // работы. Это и потеря денег у честного игрока, и печатный станок для нечестного
    // (перевести деньги сообщнику до спавна, затем «восстановиться» из кэша).
    // Реальный баланс применяет сама загрузка (PlayerMoneyPersistSystem::loadMoney),
    // спавн его не сбрасывает; если загрузка ещё в пути — применит наблюдатель.
    // HUD на спавне и без нас приводит PlayerMoneySystem::onPlayerSpawn
    // (syncToClient — пуш ТЕКУЩЕГО баланса, без записи состояния).
    if (!m_moneyPersistService.isMoneyLoaded(playerId))
        m_awaitingMoneyApply[playerId] = true;

    if (m_weaponPersistService.areWeaponsLoaded(playerId))
    {
        for (const auto &[weaponId, ammo] : m_weaponPersistService.cachedWeapons(playerId))
            m_weaponService.giveWeapon(player, weaponId, static_cast<std::uint32_t>(ammo));
        // Взводим ПОСЛЕ реальной выдачи: PlayerWeaponPersistSystem гейтит save
        // оружия этим флагом, а не areWeaponsLoaded (см. Docs/Persistence.md).
        m_weaponPersistService.markWeaponsApplied(playerId);
    }
    else
        m_awaitingWeaponsApply[playerId] = true;
}
