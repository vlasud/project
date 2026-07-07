#include "Systems/PortJobSystem/PortJobSystem.h"

#include "Database/DatabaseManager.h"
#include "Log/LogManager.h"
#include "Utils/Encoding/Encoding.h"
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <mysqlx/xdevapi.h>
#include <random>

namespace
{
// Игровые цвета сообщений (как в ParkingSystem/PaymentSystem).
const Colour INFO_COLOUR{120, 220, 255};
const Colour ERROR_COLOUR{255, 90, 90};

// Пикап порта (серверный, фикс.). Исходный facing 181.8998 — ориентир, пикапу
// угол не нужен. Модель/тип — как у пикапа парковки (info-икона «i», сервис-точка,
// подбор по касанию, всегда видна): единая визуальная конвенция для «встал ->
// открылось меню», не машина-икона.
const Vector3 PORT_PICKUP_POS{2746.6726f, -2450.3950f, 13.6484f};
constexpr int PORT_PICKUP_MODEL = 1239;
constexpr PickupType PORT_PICKUP_TYPE = 1;

// Источник ящиков — чекпоинт корабля (один, персональный per-player).
const Vector3 SOURCE_POS{2809.5854f, -2436.2959f, 13.6283f};

// 6 точек сброса склада — персональные (каждому назначается ровно одна текущая).
const Vector3 DROP_POSITIONS[PortJobService::DROP_COUNT] = {
    {2793.9033f, -2464.2310f, 13.6322f}, {2785.7219f, -2449.6755f, 13.6342f},
    {2793.5857f, -2410.9824f, 13.6322f}, {2785.9136f, -2424.7654f, 13.6341f},
    {2793.3564f, -2502.0574f, 13.6435f}, {2785.3872f, -2487.2617f, 13.6532f},
};

// Стандартный радиус персонального чекпоинта в проекте (см. CheckpointService.h).
constexpr float CHECKPOINT_RADIUS = 3.0f;

// Ящик в руке: модель случайно из пула (визуальное разнообразие), правая рука.
// ВНИМАНИЕ: offset/rotation — стартовое приближение, требует проверки вживую на
// клиенте (как читается ящик в руках) — verify-on-client, дизайнер/дев уточнят.
constexpr int BOX_MODELS[] = {1220, 1221, 1230};
const Vector3 BOX_OFFSET{0.0f, 0.0f, -0.02f};
const Vector3 BOX_ROTATION{0.0f, 0.0f, 0.0f};
// Дефолтный ящик в руке слишком крупный — уменьшаем до 2/3 (MIN_SCALE=0.01, не клампится).
const Vector3 BOX_SCALE{2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f};

int randomBoxModel()
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<int> dist(0, static_cast<int>(std::size(BOX_MODELS)) - 1);
    return BOX_MODELS[dist(rng)];
}

// Удержание позы подъёма/укладки: анимация проигрывается ОДИН раз (freeze=1, time=0)
// и застывает на финальном кадре, а по этому таймеру берётся/кладётся ящик и идёт
// переход. time анимации НЕ равен этим значениям специально — раньше time>натуральной
// длины заставлял клиента повторять анимацию 2-3 раза, пока не выйдет время.
constexpr Milliseconds LIFT_DURATION{1100};
constexpr Milliseconds PUTDOWN_DURATION{1000};

// Экранные попапы (ScreenNoticeService, поверх textdraw, не чат) — старт смены
// и сдача ящика. Длительность заметна, но не залипает поверх геймплея. Цвет —
// из общего свода цветов попапов (Docs/GameDesign/UI_Texts.md): белый —
// нейтральный статус без порога механики за ним (старт смены), зелёный —
// положительное событие (тот же #90EE90, что «congratulations!» на создании
// семьи). INFO_COLOUR/ERROR_COLOUR ниже — палитра ЧАТА, для попапов не берём:
// у попапов свой, отдельно задокументированный набор цветов.
// Текст попапов — на английском (как все прочие экранные попапы проекта:
// GreetingSystem/FamilySystem/VehicleEngineNotice), ASCII — u()/cp1251 не нужен.
constexpr Milliseconds START_WORK_POPUP_TIME{3000};
constexpr Milliseconds DELIVERY_POPUP_TIME{3000};
const Colour DELIVERY_POPUP_COLOUR{0x90, 0xEE, 0x90, 0xFF};

std::string u(const std::string &text)
{
    return Encoding::utf8Tocp1251(text);
}

Dialog makeDialog(DialogStyle style, const std::string &title, const std::string &body, const std::string &leftButton,
                  const std::string &rightButton)
{
    Dialog dialog;
    dialog.style = style;
    dialog.title = u(title);
    dialog.body = u(body);
    dialog.leftButton = u(leftButton);
    dialog.rightButton = u(rightButton);
    return dialog;
}
} // namespace

PortJobSystem::PortJobSystem(ICore &core, const ServiceRegister &serviceRegister)
    : BaseSystem(core, serviceRegister),
      m_portJobService(serviceRegister.getService<PortJobService>()),
      m_portWalletService(serviceRegister.getService<PortWalletService>()),
      m_pickupService(serviceRegister.getService<PickupService>()),
      m_checkpointService(serviceRegister.getService<CheckpointService>()),
      m_animationService(serviceRegister.getService<PlayerAnimationService>()),
      m_attachmentService(serviceRegister.getService<AttachmentService>()),
      m_moneyService(serviceRegister.getService<PlayerMoneyService>()),
      m_stateService(serviceRegister.getService<PlayerStateService>()),
      m_healthService(serviceRegister.getService<PlayerHealthService>()),
      m_dialogService(serviceRegister.getService<PlayerDialogService>()),
      m_sessionService(serviceRegister.getService<PlayerSessionService>()),
      m_timers(serviceRegister.getService<TimerService>()),
      m_screenNoticeService(serviceRegister.getService<ScreenNoticeService>())
{
    m_boxSlot.fill(-1);

    // Загрузка персистентного кошелька порта по старту сессии (serial-guard) —
    // накопленный заработок доступен к выдаче сразу после релога.
    m_sessionService.subscribeStart([this](IPlayer &player, const PlayerSessionService::Session &session)
                                    { loadWallet(player, session); });

    // Конец сессии (в т.ч. дисконнект) в ЛЮБОЙ фазе: снять ящик/анимацию/чекпоинт и
    // обнулить ВОЛАТИЛЬНОЕ состояние смены. Кошелёк порта НЕ сгорает — он
    // персистентен в БД (PortWalletService::reset чистит только память слота,
    // баланс остаётся в БД до явного «Забрать деньги»). Ре-используемый playerId не
    // должен унаследовать чужую работу/чужой закэшированный баланс.
    m_sessionService.subscribeEnd([this](IPlayer &player, const PlayerSessionService::Session &) { resetPlayer(player); });

    // Смерть в смене серверно-авторитетна (PlayerHealthService::subscribeDeath, не
    // клиентский onPlayerDeath, который чит может не прислать): роняем несомый ящик,
    // смена продолжается.
    m_healthService.subscribeDeath([this](IPlayer &player) { onPlayerDeath(player); });

    // Респавн: чекпоинт источника переставляем на onPlayerSpawn (персональный
    // чекпоинт CheckpointService после смерти-респавна сам не перепоказывается).
    core.getPlayers().getPlayerSpawnDispatcher().addEventHandler(this);
}

void PortJobSystem::initialize(IComponentList * /*components*/)
{
    // К моменту initialize PickupSystem уже получил компонент пикапов (порядок
    // реестра систем — PortJobSystem зарегистрирована после него).
    m_pickup = m_pickupService.add(
        PORT_PICKUP_MODEL, PORT_PICKUP_TYPE, PORT_PICKUP_POS, [this](IPlayer &player) { onPickup(player); }, 0);
}

void PortJobSystem::onPickup(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    const bool working = m_portJobService.isWorking(playerId);
    // Пункт «Забрать деньги» — показывается ВСЕГДА (в смене и вне её), гейт на
    // пустой баланс/сессию — внутри обработчика, не скрытием пункта.
    const std::string body =
        fmt::format("{}\nЗабрать деньги\nИнформация", working ? "Завершить работу" : "Начать работу");

    m_dialogService.show(
        player, makeDialog(DialogStyle_LIST, "Порт — работа грузчиком", body, "Выбрать", "Закрыть"),
        [this, playerId](DialogResponse response, int listItem, StringView)
        {
            IPlayer *player = m_core.getPlayers().get(playerId);
            if (!player || response != DialogResponse_Left)
            {
                return; // игрок вышел / закрыл — ничего не делаем
            }
            if (listItem == 0)
            {
                onToggleWork(*player);
            }
            else if (listItem == 1)
            {
                onWithdrawMoney(*player);
            }
            else if (listItem == 2)
            {
                showInfo(*player);
            }
        });
}

void PortJobSystem::onToggleWork(IPlayer &player)
{
    // Реальное действие смотрит на ЖИВУЮ фазу на момент клика (не на лейбл,
    // захваченный при открытии диалога) — состояние могло смениться, пока диалог
    // был открыт.
    if (m_portJobService.isWorking(player.getID()))
    {
        onFinishWork(player);
    }
    else
    {
        onStartWork(player);
    }
}

void PortJobSystem::onStartWork(IPlayer &player)
{
    const int playerId = player.getID();

    if (!m_sessionService.isActive(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы работать"));
        return;
    }
    // Клиенту не доверяем: гейт на СЕРВЕРНОМ стейте (источник истины —
    // PlayerStateService), а не на предположении, что раз стоит на пикапе — значит
    // не в машине.
    if (m_stateService.getState(playerId) != PlayerState_OnFoot)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Выйдите из транспорта, чтобы начать работу в порту"));
        return;
    }
    if (!m_portJobService.startWork(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы уже работаете в порту"));
        return;
    }

    // Предзагружаем либу CARRY сейчас (игрок ещё идёт к кораблю ~несколько секунд) —
    // иначе ПЕРВЫЙ ApplyAnimation(LIFTUP05) на незагруженной либе не проигрывается.
    m_animationService.preloadLibrary(player, "CARRY");

    m_checkpointService.setForPlayer(player, SOURCE_POS, CHECKPOINT_RADIUS, [this](IPlayer &p) { onSourceEnter(p); });
    player.sendClientMessage(INFO_COLOUR, u("Смена начата. Идите на отмеченный чекпоинт — там груз с корабля"));
    m_screenNoticeService.show(player, "port job started", START_WORK_POPUP_TIME, Colour::White());
}

void PortJobSystem::onFinishWork(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (!m_portJobService.isWorking(playerId))
    {
        player.sendClientMessage(ERROR_COLOUR, u("Вы сейчас не работаете"));
        return;
    }

    const int delivered = m_portJobService.deliveredOf(playerId); // endWork ниже обнулит счётчик
    // Застали в фазе несения — снять ящик/анимацию (чекпоинт снимается общим
    // clearForPlayer ниже, для обеих фаз одинаково). Отмена ожидающего таймера
    // лифт/путдаун — иначе стале-колбэк мог бы продвинуть будущую смену (см.
    // комментарий у m_pendingTimer).
    m_timers.cancel(m_pendingTimer[playerId]);
    detachBox(player);
    m_animationService.stop(player);
    m_stateService.clearSpecialAction(player); // мог увольняться прямо в несении (carry)
    m_checkpointService.clearForPlayer(player);

    // Деньги НЕ выплачиваются здесь — каждая сдача уже зачислена в кошелёк
    // порта (write-through, PortWalletService::add). «Завершить работу» лишь
    // завершает смену; заработок забирается отдельным пунктом «Забрать деньги».
    m_portJobService.endWork(playerId);

    const std::int64_t walletBalance = m_portWalletService.balanceOf(playerId);
    player.sendClientMessage(
        INFO_COLOUR,
        u(fmt::format("Смена окончена. Ящиков отнесено: {}. В кошельке порта: ${} — заберите через «Забрать деньги»",
                      delivered, walletBalance)));
}

void PortJobSystem::onWithdrawMoney(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }

    // Ре-валидация на клике (диалог мог провисеть, пока сессия сменилась):
    // серверный accountId из сессии, не клиентское предположение о балансе.
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    if (!session || session->accountId == PlayerSessionService::NO_ACCOUNT)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Авторизуйтесь, чтобы забрать деньги"));
        return;
    }

    if (m_portWalletService.balanceOf(playerId) <= 0)
    {
        player.sendClientMessage(ERROR_COLOUR, u("Забирать нечего — кошелёк порта пуст"));
        return;
    }

    // withdraw обнуляет кэш СРАЗУ — повторный клик/второй колбэк того же диалога
    // не выдаст деньги дважды.
    const std::int64_t amount = m_portWalletService.withdraw(playerId, session->accountId);
    if (amount <= 0)
    {
        return; // гонка кликов — уже забрано между проверкой баланса и withdraw
    }

    m_moneyService.giveMoney(player, static_cast<unsigned long long>(amount));
    player.sendClientMessage(INFO_COLOUR, u(fmt::format("Вы забрали ${} из кошелька порта", amount)));
}

void PortJobSystem::showInfo(IPlayer &player)
{
    const int playerId = player.getID();

    // TABLIST_HEADERS: карточка «Поле | Значение» — тот же формат, что «Информация»
    // в /mn (см. MenuSystem). Просмотр (клик по строке ничего не делает).
    std::string body = "Поле\tЗначение\n";
    body += "Суть работы\tНосить ящики с корабля на склад\n";
    body += fmt::format("Ставка\t${} за ящик\n", PortJobService::PAY_PER_BOX);
    body += "Выплата\tзачисляется в кошелёк порта СРАЗУ при сдаче ящика; на руки — через «Забрать деньги»\n";
    body += fmt::format("Отнесено за смену\t{}\n", m_portJobService.deliveredOf(playerId));
    body += fmt::format("В кошельке порта\t${}", m_portWalletService.balanceOf(playerId));

    m_dialogService.show(player, makeDialog(DialogStyle_TABLIST_HEADERS, "Работа в порту", body, "Назад", ""),
                         [](DialogResponse, int, StringView) {});
}

void PortJobSystem::onSourceEnter(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    // Обработчик чекпоинта мог дозвониться после смены фазы (защитный гейт —
    // персональный чекпоинт снимается синхронно ниже, но не доверяем порядку
    // колбэков вслепую).
    if (m_portJobService.phaseOf(playerId) != PortJobService::Phase::GoToSource)
    {
        return;
    }

    m_checkpointService.clearForPlayer(player); // цель снята на время анимации — ровно одна активная цель
    // freeze=1, time=0: сыграть один раз и застыть в финальной позе (НЕ повторять,
    // чтобы заполнить time). Позу снимет onLiftFinished, когда возьмётся ящик.
    const AnimationData liftAnim(4.1f, false, true, true, true, 0, "CARRY", "LIFTUP05");
    m_animationService.play(player, liftAnim, true);

    m_pendingTimer[playerId] =
        m_timers.setPlayerTimeout(player, LIFT_DURATION, [this](IPlayer &p) { onLiftFinished(p); });
}

void PortJobSystem::onLiftFinished(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (m_portJobService.phaseOf(playerId) != PortJobService::Phase::GoToSource)
    {
        return; // смена завершена/отменена, пока играла анимация подъёма
    }

    const int spot = m_portJobService.assignDropSpot(playerId);
    if (spot < 0 || spot >= PortJobService::DROP_COUNT)
    {
        return; // недостижимо после проверки фазы выше — защитный гейт
    }

    m_animationService.stop(player); // снять застывшую позу подъёма перед несением
    m_boxSlot[playerId] = m_attachmentService.attach(player, randomBoxModel(), PlayerBone_RightHand, BOX_OFFSET,
                                                     BOX_ROTATION, BOX_SCALE);
    // slot может быть -1 (все 10 слотов заняты другой фичей) — работу не проваливаем,
    // цикл продолжается без видимого ящика; detachBox(-1) ниже безопасный no-op.

    // Несение — нативный SpecialAction_Carry, а НЕ зацикленная анимация: он позволяет
    // игроку ХОДИТЬ с ящиком (обычная ходьба/бег его не сбрасывают). Зацикленная
    // full-body CRRY_PRTIAL держала игрока на месте — двигаться было нельзя.
    // enforced=false: если игрок сам выйдет из carry (прыжок/сел в машину) — без
    // санкции, доставка всё равно гейтится входом в чекпоинт сброса.
    m_stateService.setSpecialAction(player, SpecialAction_Carry);

    m_checkpointService.setForPlayer(player, DROP_POSITIONS[spot], CHECKPOINT_RADIUS,
                                     [this](IPlayer &p) { onDropEnter(p); });
}

void PortJobSystem::onDropEnter(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (m_portJobService.phaseOf(playerId) != PortJobService::Phase::Carrying)
    {
        return;
    }

    m_checkpointService.clearForPlayer(player);
    m_stateService.clearSpecialAction(player); // выйти из carry перед анимацией укладки
    // freeze=1, time=0: сыграть один раз и застыть; позу снимет onPutdownFinished.
    const AnimationData putdownAnim(4.1f, false, true, true, true, 0, "CARRY", "PUTDWN05");
    m_animationService.play(player, putdownAnim, true);

    m_pendingTimer[playerId] =
        m_timers.setPlayerTimeout(player, PUTDOWN_DURATION, [this](IPlayer &p) { onPutdownFinished(p); });
}

void PortJobSystem::onPutdownFinished(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    if (m_portJobService.phaseOf(playerId) != PortJobService::Phase::Carrying)
    {
        return;
    }

    detachBox(player);
    m_animationService.stop(player);
    m_portJobService.completeDelivery(playerId);

    // Начислить за сданную коробку СРАЗУ в персистентный кошелёк порта
    // (write-through в БД) — краш/смерть/дисконнект после этой точки уже ничего
    // не теряют. Session может отсутствовать только если сессия завершилась
    // между чекпоинтом и таймером — тогда add() no-op по NO_ACCOUNT (сама смена
    // в этот момент уже сброшена subscribeEnd, эта строка недостижима практически).
    const PlayerSessionService::Session *session = m_sessionService.get(playerId);
    const PlayerSessionService::AccountId accountId =
        session ? session->accountId : PlayerSessionService::NO_ACCOUNT;
    m_portWalletService.add(playerId, accountId, static_cast<std::int64_t>(PortJobService::PAY_PER_BOX));

    // Попап сдачи: ставка — ТОЛЬКО из PAY_PER_BOX (владелец может её сменить, попап
    // обязан совпадать с реальной выплатой), итоги — серверный счётчик delivered и
    // ТЕКУЩИЙ баланс кошелька порта (накопленное к выдаче через «Забрать деньги»,
    // а не «выдано сейчас» — деньги на руки только по явному действию игрока).
    // Одна строка: макет попапа однострочный (перенос строки не нужен; литеральный
    // '\n' всё равно срезал бы TextDrawService::sanitizeText).
    const int delivered = m_portJobService.deliveredOf(playerId);
    const std::int64_t walletBalance = m_portWalletService.balanceOf(playerId);
    m_screenNoticeService.show(
        player,
        fmt::format("+1 box (+${}). Delivered: {} (in wallet: ${})", PortJobService::PAY_PER_BOX, delivered,
                    walletBalance),
        DELIVERY_POPUP_TIME, DELIVERY_POPUP_COLOUR);

    // Снова к источнику — бесконечный цикл смены.
    m_checkpointService.setForPlayer(player, SOURCE_POS, CHECKPOINT_RADIUS, [this](IPlayer &p) { onSourceEnter(p); });
}

void PortJobSystem::onPlayerDeath(IPlayer &player)
{
    const int playerId = player.getID();
    if (!m_portJobService.isWorking(playerId))
    {
        return; // не в смене — смерть работы не касается
    }

    // Смерть = уронил несомый груз, но смена НЕ прерывается (delivered сохраняется).
    // Отменяем висящий таймер лифт/путдаун — после респавна он не должен продвинуть
    // цикл; снимаем ящик/анимацию; откатываем фазу к GoToSource (releaseSpot внутри
    // dropCarry). Чекпоинт снимаем сейчас, заново ставим на onPlayerSpawn — ставить
    // персональный чекпоинт мёртвому игроку смысла нет (он всё равно респавнится).
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_timers.cancel(m_pendingTimer[playerId]);
    }
    detachBox(player);
    m_animationService.stop(player);
    m_stateService.clearSpecialAction(player); // уронил ящик — выйти из carry
    m_portJobService.dropCarry(playerId);
    m_checkpointService.clearForPlayer(player);
    player.sendClientMessage(ERROR_COLOUR, u("Вы уронили ящик при смерти. Возьмите новый на корабле"));
}

void PortJobSystem::onPlayerSpawn(IPlayer &player)
{
    // Работающий игрок попадает на респавн только через смерть, после которой
    // onPlayerDeath уже перевёл фазу в GoToSource — ставим чекпоинт источника заново.
    // Вне смены (NotWorking, обычный спавн после логина) — ничего не делаем.
    if (m_portJobService.phaseOf(player.getID()) != PortJobService::Phase::GoToSource)
    {
        return;
    }
    m_checkpointService.setForPlayer(player, SOURCE_POS, CHECKPOINT_RADIUS, [this](IPlayer &p) { onSourceEnter(p); });
}

void PortJobSystem::detachBox(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId < 0 || playerId >= MAX_PLAYERS)
    {
        return;
    }
    int &slot = m_boxSlot[playerId];
    if (slot >= 0)
    {
        m_attachmentService.detach(player, slot);
        slot = -1;
    }
}

void PortJobSystem::resetPlayer(IPlayer &player)
{
    const int playerId = player.getID();
    if (playerId >= 0 && playerId < MAX_PLAYERS)
    {
        m_timers.cancel(m_pendingTimer[playerId]);
    }
    detachBox(player);
    m_animationService.stop(player); // no-op, если не играла
    m_stateService.clearSpecialAction(player);  // снять carry, если несли на конце сессии
    m_checkpointService.clearForPlayer(player); // no-op, если персонального чекпоинта не было
    m_portJobService.resetPlayer(playerId);
    m_portWalletService.reset(playerId); // teardown ТОЛЬКО памяти — баланс остаётся в БД
}

void PortJobSystem::loadWallet(IPlayer &player, const PlayerSessionService::Session &session)
{
    DatabaseManager::selectQuery<std::int64_t>(
        [accountId = session.accountId](mysqlx::Schema schema) -> std::int64_t
        {
            mysqlx::RowResult result = schema.getTable("port_wallet")
                                           .select("balance")
                                           .where("account_id = :account")
                                           .limit(1)
                                           .bind("account", accountId)
                                           .execute();
            // Нет строки -> кошелёк пуст (ещё ни разу не сдавал ящик).
            if (mysqlx::Row row = result.fetchOne())
            {
                return row.get(0).get<std::int64_t>();
            }
            return std::int64_t{0};
        },
        [this, playerId = player.getID(), serial = session.serial](std::int64_t balance)
        {
            // Serial-guard: в слоте мог оказаться другой игрок/другая сессия.
            const PlayerSessionService::Session *current = m_sessionService.get(playerId);
            if (!current || current->serial != serial)
            {
                return;
            }
            m_portWalletService.load(playerId, balance);
        },
        [](const std::string &error)
        { LogManager::log(Error, "PortJobSystem: failed to load port wallet: " + error); });
}
