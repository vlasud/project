#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "Services/Core/PlayerLocationService/PlayerLocationService.h"
#include "Services/Core/VehicleService/RepairZones.h" // RepairZones::Zone — параметр dwelledInZone
#include "player.hpp"
#include "types.hpp"
#include <Server/Components/Vehicles/vehicles.hpp>
#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class GridService; // пространственный индекс машин — для anyVehicleNear (указатель-член)

// Сервис машин — источник правды о том, кто в какой машине сидит, и о серверном
// HP каждой машины, плюс валидации.
//
// HP машины клиент-авторитетно: его диктует driver sync водителя. Поэтому сервис
// ведёт принятое HP храповиком по модели игрокового здоровья: снижение (урон)
// принимается, вверх принятое HP клиент не двигает вовсе (HEALTH_EPS — лишь
// допуск «не флажить дрожание» float, рост дают только серверные операции),
// рост без серверной санкции — vehicle repair hack — откатывается и фиксируется.
// Единственный рост по клиентскому событию — санкции ремонта мод-шопа/Pay'n'Spray
// (SCM-события), и они гейтятся серверной позицией машины у известной ремзоны
// (RepairZones.h) + серверной сессией мод-шопа: голый SCM-пакет ремонта не даёт.
// Позиция машины (vehicle.getPosition()) сама по себе КЛИЕНТ-АВТОРИТЕТНА — ядро
// пишет её безусловно из driver-sync водителя (Vehicle::updateFromDriverSync),
// и суб-пороговый дрейф (телепорт медленнее, чем ловит PlayerLocationService::
// verify) даёт мгновенное появление в зоне без единого флага. Поэтому зонный
// гейт СОСТАВНОЙ: «машина в зоне СЕЙЧАС» + «непрерывно простояла там не меньше
// ZONE_DWELL_MIN» — второе требует физического подъезда и удержания, а не
// мгновенной телепортации; трек ведёт verifyHealth на каждом driver-sync
// (VehicleState::zoneEnteredAt).
//
// Unoccupied sync (физику пустой машины считает ближайший клиент) — канал чита
// для перетаскивания/телепорта чужих машин: валидируется дистанция репортера,
// величина скачка и скорость; фейковый апдейт ОТКЛОНЯЕТСЯ (не применяется ядром).
//
// Телепорт машины С ВОДИТЕЛЕМ ловится валидатором позиции игрока (позиция
// водителя следует за машиной) — здесь не дублируется. ИСКЛЮЧЕНИЕ — легальный
// заезд в мод-шоп: клиент физически переносит машину с водителем в интерьер
// шопа (universe-координаты), и это честный «телепорт» на тысячи метров без
// серверного teleport(). onModShop на ПРИНЯТОМ enter/exit выдаёт точечный грейс
// через PlayerLocationService::grantModShopTeleportGrace — иначе честный тюнер
// поймал бы forceTo и вылетел бы из шопа как читер.
//
// МАШИНЫ НЕ ТЕРЯЮТСЯ, а там, где сервер ВИДИТ HP, — и не взрываются: ниже
// ~250 HP клиент поджигает машину и затем взрывает; как только серверное HP
// падает к порогу, машина ГЛОХНЕТ: HP клампится на STALL_HEALTH (выше порога
// пожара — восстановление HP тушит уже занявшийся клиентский огонь), двигатель
// глушится и не заводится до repair(). С водителем HP ведёт verifyHealth
// (driver-sync). БЕЗ водителя ядро пишет HP из ПРИНЯТОГО unoccupied-синка
// ТОЛЬКО когда репортер — ПАССАЖИР машины (SeatID != 0, vehicle.cpp
// updateFromUnoccupied); этот кейс тушит кламп по ядровому getHealth()
// (douseUnoccupiedFire: accept-путь синка + секундный проход secondTick).
// ПО-НАСТОЯЩЕМУ ПУСТУЮ машину (репортер — ближайший клиент, SeatID == 0)
// сервер физически НЕ видит: HP из её синков ядро не применяет, серверного
// урона по ней нет — клиентский пожар не потушить, и на клиентах она ВИДИМО
// детонирует (расстрел брошенной, утопление, взрывчатка). Потери при этом
// нет: детонацию гасит контракт смерти — машина возвращается ЦЕЛОЙ респавном.
//
// КОНТРАКТ СМЕРТИ: легальная смерть машины — ТОЛЬКО серверная санкция
// (explode() ставит serverKilled). Санкционированная смерть оповещает
// died-наблюдателей (политика бизнеса: личную destroy и т.п.).
// НЕсанкционированная (клиентская детонация пустой / фейковый репорт смерти)
// died-политику НЕ применяет: первая смерть бэкофф-окна отложенным серверным
// респавном (respawnIfDead) возвращает машину ЦЕЛОЙ НА МЕСТЕ — на точке, где
// сервер зафиксировал смерть, а НЕ на spawn-точке (иначе любой застримивший
// игрок работал бы «эвакуатором», телепортируя чужие пустые машины домой
// фейковым VehicleDeath). ПОВТОРНАЯ смерть в окне UNSANCTIONED_DEATH_BACKOFF
// (после возврата умерла снова: утопленная, спам-грифинг) наш респавн НЕ
// взводит — машину вернёт ядровой death-таймер (game.vehicle_respawn_time,
// ~10 с) на её spawn-точку: это и спасение утопленной, и потолок темпа
// стрим-чёрна. САМ репорт смерти НАРУШЕНИЕМ не считается (сервер не видит
// урона по пустой машине, честная детонация по серверным фактам неотличима
// от фейка), но НЕЧЕЛОВЕЧЕСКИЙ темп репортов от одного игрока — уже серверный
// факт и флажится (DEATH_REPORT_MAX за скользящее окно).
//
// subscribeUnsanctionedDeath оповещает бизнес о ФАКТЕ несанкционированной
// смерти (без текста — Core политику не решает): бизнес резолвит владельца по
// серверным записям и шлёт уведомление, только если тот онлайн.
//
// КОНТРАКТ: серверные изменения HP/ремонт — только через сервис; прямой
// vehicle.setHealth() мимо него валидатор посчитает читерским ростом.
class VehicleService final : public IService
{
  public:
    // Полное HP машины SA (максимум; repair чинит сюда). Единый источник факта
    // «полное HP = 1000» — на него опираются и стейт машины, и индикаторы HUD.
    static constexpr float MAX_HEALTH = 1000.0f;

    // Порог «заглохла»: с запасом выше клиентского порога пожара (250).
    static constexpr float STALL_HEALTH = 300.0f;

    // Бак (баланс — тюнится): полный объём и расход за секунду при заведённом
    // двигателе. 100 / 0.1 = 1000 с ≈ 17 минут (16 мин 40 с) езды до пустого.
    static constexpr float FUEL_CAPACITY = 100.0f;
    static constexpr float FUEL_DRAIN_PER_SEC = 0.1f;

    // Владелец машины — НЕпрозрачный серверный тег. Ядро лишь хранит пару
    // (тип, id); политику (кто что спавнит, доступ, персист) решают бизнес-
    // системы поверх. Клиент тег не задаёт.
    //
    // Parked — припаркованная у дома машина (личная owner-only ИЛИ расшаренная
    // семье). Тег лишь маркирует, что PersonalVehicleSystem её при смерти НЕ
    // удаляет (ранний return не по Owner::Player) — ядро переспавнит её на
    // spawn-позиции = точке у дома. Доступ (кто за руль) решает driver-gate по
    // записи ParkedVehicleService (по vehicleId), НЕ по ownerId тега.
    enum class Owner
    {
        None,
        Player,
        Faction,
        Work,
        Parked
    };

    // Инициализация: привязывает пул машин и сервисы; регистрирует vehicleEvents и
    // poolEvents в диспатчерах компонента (VehicleSystem передаёт себя как оба).
    // grid — пространственный индекс (anyVehicleNear спрашивает соседей машин у него,
    // а не проходом по пулу). Все сервисы сконструированы до фазы initialize, так что
    // grid уже валиден, даже если GridService в реестре идёт после VehicleService.
    void bind(IVehiclesComponent *vehicles, PlayerLocationService &location, GridService &grid,
              VehicleEventHandler &vehicleEvents, PoolEventHandler<IVehicle> &poolEvents);

    // Единая точка создания машин (источник правды): создаёт машину в пуле и
    // проставляет владельца. Возвращает nullptr, если пул полон. Стейт машины
    // (exists, HP, полный бак) уже проинициализирован пул-событием создания.
    // Всё создание машин — через этот API; onVehicleCreated остаётся ловушкой
    // для сторонних созданий (owner=None).
    IVehicle *create(int model, Vector3 position, float angle, int colour1, int colour2, Owner owner, int ownerId);

    // Получить машину пула по id (nullptr — нет компонента/несуществующая).
    IVehicle *get(int vehicleId) const;
    // Имя модели машины по vehicle id (каталог VehicleModelNames, статические
    // литералы — O(1), без аллокаций). Пустой string_view — несуществующая машина.
    std::string_view getModelName(int vehicleId) const;
    // Есть ли существующая машина в радиусе от точки (кроме excludeVehicleId).
    // Близость ГОРИЗОНТАЛЬНАЯ — по XY, Z игнорируется (точки занятости на одной
    // высоте, машина оседает на грунт со своей Z; иначе Z-разница ложно вышибала бы
    // машину из радиуса). Сравнение по квадрату дистанции (без sqrt). Соседей даёт
    // GridService (пространственный индекс) — O(машин в соседних ячейках), не проход
    // по всему пулу. Нет грида/компонента -> false.
    bool anyVehicleNear(Vector3 position, float radius, int excludeVehicleId = -1) const;
    // Уничтожить машину пула. No-op без компонента/машины. Пул-событие
    // уничтожения (через VehicleSystem) сбросит стейт и оповестит наблюдателей.
    void destroy(int vehicleId);

    // Наблюдатели жизненного цикла машин — для систем со своим индексом машин.
    // created — после регистрации стейта новой машины;
    // destroyed — пока машина ещё валидна, перед сбросом стейта;
    // died — ТОЛЬКО серверно-САНКЦИОНИРОВАННАЯ смерть (explode(), serverKilled);
    // машина ещё валидна. Бизнес может реагировать (напр., убрать личную машину,
    // чтобы она не висела вреком). Несанкционированная смерть сюда НЕ доходит —
    // её гасит сам сервис возвратом машины целой респавном (onVehicleDeath).
    // Это ОБЩАЯ инфраструктура: Core лишь оповещает о событии смерти, без
    // бизнес-политики.
    using VehicleObserver = std::function<void(IVehicle &)>;
    void subscribeCreated(VehicleObserver observer);
    void subscribeDestroyed(VehicleObserver observer);
    void subscribeDied(VehicleObserver observer);
    // Машина переспавнилась (onVehicleRespawn уже проставил ПОЛНЫЙ бак/HP —
    // источник правды об «умолчательном» состоянии свежей жизни машины). Крючок
    // для бизнеса, которому нужно восстановить ПЕРСИСТЕНТНЫЙ остаток топлива
    // поверх дефолта (setFuel поверх уже примененного FUEL_CAPACITY) — напр.
    // после несанкционированной смерти (respawnIfDead) машина должна вернуться с
    // ТЕМ ЖЕ баком, что был перед детонацией, а не долитая бесплатно. Зовётся
    // ПОСЛЕ применения дефолтов, машина валидна. Главный поток, событийно.
    void subscribeRespawned(VehicleObserver observer);

    // Наблюдатель НЕсанкционированной смерти (контракт смерти) — только ФАКТ
    // события, никакого текста: бизнес сам резолвит владельца и шлёт сообщение.
    // returnsInPlace=true — первая смерть волны, respawnIfDead взведён, машина
    // вернётся ЦЕЛОЙ НА МЕСТЕ (~100 мс); false — повторная смерть в бэкофф-окне
    // (машина уже вернулась и умерла снова), её вернёт ядровой death-таймер
    // (~10 с) на spawn-точку. Зовётся из onVehicleDeath ПОСЛЕ решения о
    // возврате — машина ещё валидна (died-lock диспатча смерти держит её
    // живой). Антиспам — одна строка на «мёртвую фазу» машины (взводится на
    // первом вызове волны, снимается respawn'ом): читер, спамящий setDead,
    // не размножит колбэк на каждый повторный диспатч onVehicleDeath.
    // ВАЖНО подписчикам: наблюдатель вызывается ИЗ death-диспатча ядра — НЕЛЬЗЯ
    // трогать пул машин (destroy/respawn/create) внутри колбэка, только читать
    // стейт и слать сообщения игрокам.
    using UnsanctionedDeathObserver = std::function<void(IVehicle &, bool returnsInPlace)>;
    void subscribeUnsanctionedDeath(UnsanctionedDeathObserver observer);

    // Наблюдатель момента опустошения бака ПОД ВОДИТЕЛЕМ (двигатель заглох сам,
    // на ходу) — только ФАКТ, без текста: Core не решает бизнес-сообщения. Зовётся
    // ОДИН раз на переход в outOfFuel (secondTick, driverId >= 0 в момент дренажа);
    // повторно не шлётся, пока бак не наполнят и не опустеет заново. driverId —
    // серверный водитель машины на момент опустошения (getDriver). Главный поток,
    // из секундного таймера — не per-tick.
    using FuelEmptyObserver = std::function<void(int vehicleId, int driverId)>;
    void subscribeFuelEmpty(FuelEmptyObserver observer);

    // Наблюдатель момента ПОЛОМКИ двигателя ПОД ВОДИТЕЛЕМ (HP добит до порога —
    // машина заглохла, двигатель не заводится до repair) — только ФАКТ, без текста
    // (Core бизнес-сообщений не решает). Зовётся ОДИН раз на ПЕРЕХОД в stalled
    // (stallIfCritical, driverId >= 0 в этот момент); повторно не шлётся, пока не
    // починят и снова не сломают. driverId — серверный водитель. Зов из
    // verifyHealth/applyDamage — НЕ трогать пул машин из колбэка, только слать текст.
    using EngineBrokenObserver = std::function<void(int vehicleId, int driverId)>;
    void subscribeEngineBroken(EngineBrokenObserver observer);

    // Вето на посадку ЗА РУЛЬ — общая инфраструктура доступа к машине (Core лишь
    // предоставляет крючок; политику — членство/оплата/бан — решает бизнес). Зовётся
    // из bindOccupant в момент, когда игрок стал водителем (seat==0). Наблюдатель
    // возвращает false, чтобы ОТКАЗАТЬ — сервис высадит игрока (removeFromVehicle,
    // force: отменяет и уже занятое место, и незавершённый вход). Пассажиров НЕ
    // гейтит — зовётся только на driver-ветке. Наблюдатель не должен трогать occupancy
    // машины (высадку делает сервис после вето). Главный поток, событийно.
    using DriverGateObserver = std::function<bool(IPlayer &, IVehicle &)>;
    void subscribeDriverGate(DriverGateObserver observer);

    // Наблюдатель смены позиции БЕЗ водителя — для систем с пространственным
    // индексом машин (GridService): под водителем грид ведёт driver-апдейт, а на
    // прочих путях смены реальной позиции (респаун, ПРИНЯТЫЙ unoccupied-синк)
    // машина «телепортируется» помимо него, и индекс становится стейл. Зовётся
    // ТОЛЬКО с серверно-ПРИНЯТОЙ позицией: на респауне — getPosition() (ядро уже
    // на spawn-позиции), на unoccupied — позиция ИЗ принятого апдейта (ядро
    // применит её ПОСЛЕ accept, getPosition() в этот момент ещё старая).
    // Отклонённый читерский апдейт сюда не доходит. Главный поток.
    using VehicleMoveObserver = std::function<void(IVehicle &, Vector3 acceptedPosition)>;
    void subscribeMoved(VehicleMoveObserver observer);

    // Наблюдатель ПРИМЕНЁННОГО СЕРВЕРОМ тюнинга (компонент/пейнтджоб/цвет) —
    // зовётся ТОЛЬКО из notifyServerTuned, которую явно дёргает вызывающий бизнес
    // ПОСЛЕ installComponent/setColour/setPaintJob (дев-команда, будущее игровое
    // меню). Клиентский мод-гараж/Pay'n'Spray тюнинг больше НЕ подтверждают —
    // validateMod/validatePaintJob/validateRespray их отклоняют всегда (см. ниже),
    // сюда они не ведут. Только ФАКТ, без пула машин: бизнес, персистящий внешний
    // вид (см. PersonalVehicleService), может снять снимок сразу, не дожидаясь
    // деспавна — тюнинг переживёт краш до штатного уничтожения экземпляра. Главный
    // поток, событийно (тюнинг — редкое действие, не per-tick).
    using TunedObserver = std::function<void(IVehicle &)>;
    void subscribeTuned(TunedObserver observer);
    // Оповестить subscribeTuned о СЕРВЕРНО применённом тюнинге. В отличие от
    // прежнего клиентского accept-пути (когда ядро применяло мод/пейнтджоб/цвет
    // синхронно ПОСЛЕ возврата из event-хендлера), installComponent/setColour/
    // setPaintJob — прямые вызовы: к моменту этого вызова состояние машины УЖЕ
    // отражает изменение, наблюдатель может читать getComponentInSlot/getColour/
    // getPaintJob хоть синхронно. Вызывающий сам решает, когда его звать — низкие
    // addComponent/setPaintJob/setColour его НЕ зовут (см. их комментарий), иначе
    // переприменение персиста на спавне дало бы ре-энтрантный снимок.
    void notifyServerTuned(IVehicle &vehicle);

    // Машина застримлена конкретному игроку (VehicleEventHandler::onVehicleStreamIn,
    // проброшено VehicleSystem). Ядро НЕ восстанавливает пер-игроковые params
    // (setParamsForPlayer) при повторном стрим-ине — бизнес, владеющий пер-игроковым
    // состоянием (напр. замок дверей VehicleLockService), обязан переприменить его
    // здесь. Главный поток, событийно (per-tick нет — стрим-ин редок).
    using StreamedInForPlayerObserver = std::function<void(IVehicle &, IPlayer &)>;
    void subscribeStreamedInForPlayer(StreamedInForPlayerObserver observer);

    // --- источник правды ---
    IVehicle *getVehicle(int playerId) const; // машина игрока (по принятому стейту)
    int getSeat(int playerId) const;          // -1 — не в машине; 0 — водитель
    int getDriver(int vehicleId) const;       // id водителя или -1
    float getHealth(int vehicleId) const;     // серверное HP машины
    bool isStalled(int vehicleId) const;      // заглохла (HP добит до порога)
    // Клиентская велосити машины (SA-юниты) — то, что водитель заявил в driver
    // sync. КОСМЕТИКА (спидометр на экране самого водителя), НЕ для логики:
    // значение подделываемо, серверная правда о движении — PlayerVelocityService.
    // {0,0,0} для несуществующей машины.
    Vector3 getVelocity(int vehicleId) const;

    // --- владелец (серверный тег, читается бизнес-логикой) ---
    Owner getOwner(int vehicleId) const;   // None для несуществующей/чужой
    int getOwnerId(int vehicleId) const;   // -1 при отсутствии владельца
    void setOwner(IVehicle &vehicle, Owner owner, int ownerId);

    // --- топливо ---
    float getFuel(int vehicleId) const;     // 0 для несуществующей машины
    bool isOutOfFuel(int vehicleId) const;  // пустой бак — двигатель не заводится
    void refuel(IVehicle &vehicle, float amount); // долить (amount>0), кламп на CAP
    void setFuel(IVehicle &vehicle, float amount); // абсолют, кламп 0..CAP
    // Секундный обслуживающий проход по пулу (таймер VehicleSystem, не per-tick),
    // одним циклом два дела:
    //  * тушение машин БЕЗ водителя: ядровое getHealth() ниже порога -> кламп +
    //    глушим (douseUnoccupiedFire), broadcast setHealth тушит огонь у
    //    симулирующего клиента. HP в ядро без водителя пишет только принятый
    //    unoccupied-синк ПАССАЖИРА (SeatID != 0) — фактическое покрытие
    //    «пассажир без водителя»; по-настоящему пустой ядровое HP ниже клампа
    //    никто не опускает (для неё это no-op, детонацию возвращает контракт
    //    смерти). Стоящая на клампе машина RPC не генерит;
    //  * дренаж бака за прошедшие seconds, расход по факту РАБОТАЮЩЕГО двигателя:
    //    engine==1 жжёт всегда (в т.ч. оставленная заведённой без водителя),
    //    engine==-1 (клиентский авто-режим, дефолт) — только при водителе за
    //    рулём; engine==0 и -1 без водителя не расходуют (нетронутая парковка не
    //    пустеет сама). Пустой бак глушит двигатель И, если под водителем —
    //    зовёт subscribeFuelEmpty (только факт; текст шлёт бизнес).
    void secondTick(float seconds);

    // --- серверные операции ---
    void setHealth(IVehicle &vehicle, float health);
    // ПРИВИЛЕГИРОВАННАЯ дев-операция: форсирует смерть машины в обход анти-грифинга.
    // Ставит serverKilled (санкция смерти: died-наблюдатели сработают, douse-кламп
    // её не воскресит), серверное HP в 0 и шлёт клиенту setHealth(0) БЕЗ
    // stallIfCritical, чтобы клиент детонировал машину, отрепортил смерть и сервер
    // диспатчнул onVehicleDeath. Обычный урон по-прежнему глохнет (stallIfCritical /
    // анти-грифинг не трогаем) — это исключение только для теста реального уничтожения.
    void explode(IVehicle &vehicle);
    void repair(IVehicle &vehicle); // полный ремонт: HP 1000 + визуал + снимает «заглохла»
    // Серверно-авторитетный урон машине (стрельба: клиент водителя чужих пуль не
    // видит при lagcomp — урон применяет сервер; модель ровно как HP игрока).
    void applyDamage(IVehicle &vehicle, float amount);
    void setEngine(IVehicle &vehicle, bool on); // заглохшую завести нельзя (сначала repair)
    void setLights(IVehicle &vehicle, bool on); // фары можно переключать всегда
    void setLocked(IVehicle &vehicle, bool locked);
    // Пер-игроковый замок дверей (IVehicle::setParamsForPlayer, params.doors) — в
    // отличие от setLocked (глобальный params, блокирует ВСЕХ включая владельца),
    // этот вызов трогает состояние двери ТОЛЬКО для указанного player. Политику
    // (кому заперто) решает бизнес (VehicleLockService); Core лишь даёт примитив.
    // Ядро НЕ восстанавливает пер-игроковые params на повторном стрим-ине клиенту —
    // бизнес обязан переприменять их через subscribeStreamedInForPlayer.
    void setLockedForPlayer(IVehicle &vehicle, IPlayer &player, bool locked);
    // Застримлена ли машина указанному игроку сейчас (обёртка isStreamedInForPlayer
    // SDK) — бизнес не должен звать сырой SDK напрямую.
    bool isStreamedInForPlayer(const IVehicle &vehicle, const IPlayer &player) const;
    // Сменить ТОЛЬКО spawn-позицию/угол существующей машины (для death-респавна на
    // её ТЕКУЩУЮ точку), сохранив прочие поля spawnData (модель/цвета/respawnDelay/
    // siren/interior). Через get/setSpawnData — БЕЗ пересоздания машины: ре-тег
    // парковки НА МЕСТЕ должен вернуть машину сюда, если ядро её переспавнит по смерти.
    // Мусорную (NaN/Inf) позицию/угол в spawnData не пускаем. No-op для несуществующей.
    void setSpawnPosition(IVehicle &vehicle, Vector3 position, float angle);

    // Обёртка vehicle.respawn() для бизнес-политики «вернуть на spawn-точку по
    // требованию» (напр. /car -> «Респавн» припаркованной у дома). respawn() ядра
    // САМ ДАЁТ ПОЛНЫЙ БАК/HP (см. onVehicleRespawn) — вызывающий бизнес, которому
    // нужно сохранить персистентный остаток топлива, обязан снять снимок ДО этого
    // вызова и восстановить его через subscribeRespawned (см. ParkedVehicleSystem).
    // No-op для несуществующей/нулевой машины.
    void respawn(IVehicle &vehicle);

    // Байпас валидации unoccupied-синка для машины, которую легально двигает
    // сервер (редактор мира): телепорты машины — серверная правда, а
    // редактирующий игрок (репортер синка) может быть телом далеко от камеры.
    // Включается редактором на время жизни сущности, сбрасывается при
    // уничтожении машины.
    void setEditBypass(int vehicleId, bool enable);

    // Серверный тюнинг — ЕДИНСТВЕННЫЙ источник (бизнес-логика тюнинг-салонов,
    // дев-команда, будущее игровое меню). Это ПРЯМЫЕ вызовы IVehicle-примитива
    // (широковещательный RPC клиентам), НЕ клиентские SCM-заявки: клиентский
    // нативный мод-гараж/Pay'n'Spray как ИСТОЧНИК тюнинга ОТКЛЮЧЕНЫ —
    // validateMod/validatePaintJob/validateRespray теперь ВСЕГДА отклоняют
    // событийный путь onVehicleMod/onVehiclePaintJob/onVehicleRespray (см. их
    // комментарии), ядро не применяет клиентский мод/пейнтджоб/цвет ни в каком
    // случае. Персист внешнего вида (см. PersonalVehicleService) применяет
    // сохранённый тюнинг именно этими вызовами.
    //
    // Диапазон/валидность компонента (1000..1193 И подходит модели —
    // isValidComponentForVehicleModel) и пейнтджоба (0..2) — мусор извне
    // (дев-команда/персист/будущее UI) тихо отбрасывается, без краша.
    void addComponent(IVehicle &vehicle, int component);
    void removeComponent(IVehicle &vehicle, int component);
    void setPaintJob(IVehicle &vehicle, int paintjob);

    // Установить компонент с ЯВНОЙ заменой слота: валидирует диапазон/модель,
    // резолвит слот компонента (getVehicleComponentSlot) и явно СНИМАЕТ текущий
    // компонент этого слота (если был и отличается) ПЕРЕД установкой нового —
    // «один компонент на слот» с явным RemoveVehicleComponent-RPC старой детали
    // клиентам, а не расчёт на неявную перезапись mods[slot] внутри ядрового
    // addComponent. Мусорный/невалидный для модели компонент — no-op.
    // Предпочтительный способ поставить тюнинг-компонент.
    void installComponent(IVehicle &vehicle, int component);

    // Число слотов компонентов машины (SDK MAX_VEHICLE_COMPONENT_SLOT — 16: 14
    // стримятся в стандартном stream-in RPC + 2 доп. бампера отдельным SCM).
    // Единый источник правды диапазона getComponentInSlot/снимка компонентов.
    static constexpr int COMPONENT_SLOT_COUNT = MAX_VEHICLE_COMPONENT_SLOT;

    // Цвет машины (обёртка IVehicle::getColour) — {-1,-1} для несуществующей.
    // Для снятия снимка персистом внешнего вида (бизнес не зовёт сырой SDK).
    std::pair<int, int> getColour(int vehicleId) const;
    // Серверная смена цвета (обёртка IVehicle::setColour) — для дев-команды/
    // будущего UI и применения сохранённого снимка (сейчас цвет также задаётся
    // через create). Диапазон 0..255 на канал — вне диапазона no-op (иначе ядро
    // молча маскирует & 0xFF, что вводит в заблуждение вызывающего).
    void setColour(IVehicle &vehicle, int colour1, int colour2);

    // Пейнтджоб машины (обёртка IVehicle::getPaintJob) — -1 для несуществующей
    // машины ЛИБО отсутствия пейнтджоба (ядро само хранит -1 = «нет пейнтджоба»
    // после -1 back-conversion в Vehicle::getPaintJob).
    int getPaintJob(int vehicleId) const;

    // Снимок установленных компонентов машины: обходит слоты 0..COMPONENT_SLOT_COUNT-1
    // (getComponentInSlot), непустые (!=0) id складывает в out. out очищается перед
    // заполнением. Для несуществующей машины out остаётся пустым. O(слотов) — холодный
    // путь (снятие снимка на спавне/деспавне/событии мода, не per-tick).
    void getComponents(int vehicleId, std::vector<int> &out) const;

    // --- удобное API перечисления тюнинга (для меню без ручного ввода id) ---

    // Слот компонента (обёртка Impl::getVehicleComponentSlot из SDK) — id вне
    // диапазона 1000..1193 даёт VehicleComponent_None (-1), как и мусорный id
    // внутри диапазона у самого SDK. Бизнес-слой строит по слоту меню/подпись.
    int getComponentSlot(int component) const;

    // Все компоненты диапазона 1000..1193, которые ОДНОВРЕМЕННО принадлежат
    // указанному слоту (getVehicleComponentSlot) И валидны для модели
    // (isValidComponentForVehicleModel) — ядро удобного API «какие детали слота
    // подходят этой машине». out очищается перед заполнением; невалидный slot
    // (вне 0..COMPONENT_SLOT_COUNT-1) оставляет out пустым. O(диапазона
    // компонентов, максимум 194) — холодный путь построения меню, не per-tick.
    void componentsForSlot(int model, int slot, std::vector<int> &out) const;

    // Компонент, установленный в указанном слоте машины (getComponentInSlot):
    // 0 — слот пуст, -1 — несуществующая машина или мусорный slot. Для показа
    // «сейчас стоит» и определения, что снимать при установке новой детали.
    int installedInSlot(int vehicleId, int slot) const;

    struct Outcome
    {
        bool vehicleHack = false;
        // Несанкционированная смерть: назначить отложенный respawnIfDead (возврат
        // целой НА МЕСТЕ). Взводится один раз на волну смертей (спам setDead
        // диспатчит смерть каждый тик — таймеры не плодим) и только для ПЕРВОЙ
        // волны в бэкофф-окне; повторную возвращает ядровой death-таймер.
        bool queueRespawn = false;
        std::string detail;
    };

    // --- вызывается VehicleSystem ---
    void bindOccupant(IPlayer &player, PlayerState newState); // на смене стейта
    // На апдейте водителя: сверка occupant с реальной машиной ядра (форженный
    // driver-sync на другую застримленную машину пересаживает в ядре БЕЗ
    // стейт-чейнджа — расхождение перепривязывается через bindOccupant, включая
    // прогон driver-gate: вето высадит) + валидация HP. Принятое HP — храповик:
    // движется только вниз; HEALTH_EPS — допуск «не флажить дрожание», не
    // источник роста. Рост — только серверные операции (repair/setHealth/...).
    // ПОПУТНО обновляет zone dwell-трек (VehicleState::zoneEnteredAt) — момент
    // непрерывного входа машины в известную ремзону: единственное место, где
    // vehicle.getPosition() (клиент-авторитетная, ядро пишет её безусловно из
    // driver-sync) читается на КАЖДОМ апдейте, а не разово в момент SCM-события.
    // SCM-гейты (validateMod/validatePaintJob/validateRespray/onModShop) требуют
    // не только «машина в зоне сейчас», но и «непрерывно не меньше ZONE_DWELL_MIN»
    // — суб-пороговый дрейф позиции (телепорт ниже VEHICLE_MAX_SPEED игрока,
    // остающийся под радаром PlayerLocationService::verify) даёт мгновенное
    // появление в зоне, но не даёт мгновенного dwell.
    Outcome verifyHealth(IPlayer &player, TimePoint now);
    // vehicleHack в Outcome означает «апдейт отклонить» (система вернёт false ядру).
    Outcome validateUnoccupied(IVehicle &vehicle, IPlayer &reporter, const UnoccupiedVehicleUpdate &update,
                               TimePoint now);
    Outcome validateTrailer(IPlayer &reporter, IVehicle &trailer, TimePoint now);

    // Клиентская заявка на мод (AddComponent SCM) — ИСТОЧНИКОМ тюнинга больше не
    // является: клиентский нативный мод-гараж отключён, тюнинг только серверный
    // (installComponent/дев-команда/будущее меню). VehicleSystem::onVehicleMod
    // ВСЕГДА возвращает false ядру (мод не применяется), но ЭТА функция различает
    // ДВА мотива отказа:
    //  * заявка НЕ прошла бы старый гейт (не водитель / битый id компонента / вне
    //    серверно-подтверждённой сессии мод-шопа с dwell ZONE_DWELL_MIN) — это
    //    чит-меню (нитро/гидравлика где угодно): vehicleHack=true, нарушение;
    //  * заявка ПРОШЛА БЫ старый гейт (честный водитель, честно заехавший в
    //    мод-гараж и выбравший деталь) — легитимный игрок, просто клиентский
    //    путь тюнинга запрещён: vehicleHack=false, БЕЗ нарушения. sanctionRepair
    //    здесь больше НЕ зовём (мод-шоп уже чинит на onModShop-enter).
    Outcome validateMod(IPlayer &player, IVehicle &vehicle, int component, TimePoint now);
    // Заявка на пейнтджоб (SetPaintjob SCM, ядро само его НЕ гейтит — дефолтная
    // onVehiclePaintJob в SDK возвращает true) — та же ЛОГИКА ДВУХ МОТИВОВ отказа,
    // что и validateMod (та же зона/сессия/диапазон 0..2): вне зоны/не водитель —
    // vehicleHack (чит), в зоне легально — reject без нарушения (клиентский
    // пейнтджоб больше не источник). Санкцию ремонта здесь не зовём (её и раньше
    // не повторяли — пейнтджоб её не даёт и на честном клиенте).
    Outcome validatePaintJob(IPlayer &player, IVehicle &vehicle, int paintJob, TimePoint now);
    // Перекраска (SetColour SCM) — та же логика двух мотивов: вне ремзоны/не
    // водитель — vehicleHack (чит), у ремзоны (Pay'n'Spray либо мод-шоп в сессии)
    // с dwell ZONE_DWELL_MIN — легитимно, но ОТКЛОНЯЕМ (клиентский Pay'n'Spray/
    // мод-шоп больше не источник цвета). sanctionRepair НА ЛЕГИТИМНОЙ ветке
    // СОХРАНЁН: Pay'n'Spray чинит машину на клиенте НЕЗАВИСИМО от того, применит
    // ли сервер SetColour (это встроенное поведение движка GTA:SA в зоне, не
    // завязанное на исход SCM-пакета) — без sanctionRepair честный визит перестал
    // бы чинить машину, а следующий driver-sync с полным HP словил бы repair-hack.
    Outcome validateRespray(IPlayer &player, IVehicle &vehicle, TimePoint now);
    // Вход/выход мод-шопа (клиентский SCM): вход принимается только с машиной,
    // НЕПРЕРЫВНО простоявшей у ворот известного шопа (RepairZones) не меньше
    // ZONE_DWELL_MIN, и открывает сессию inModShop; выход — только для открытой
    // сессии И с машиной в зоне шопа (интерьер либо ворота) прямо сейчас (dwell
    // на выход не требуем — машина уже отстояла его на входе, а интерьер шопа
    // покидается практически сразу после решения игрока). Подавленный честный
    // exit не увозит сессию «в кармане» для ремонта в бою. Оба принятых дают
    // санкцию ремонта (шоп чинит машину на клиенте) И грейс позиции игрока
    // (grantModShopTeleportGrace) — клиент физически переносит машину с
    // водителем в интерьер/обратно, не через серверный teleport(). Событие вне
    // зоны / без выдержки на входе / выход без входа — нарушение без ремонта и
    // без грейса; выход вне зоны заодно закрывает сессию.
    Outcome onModShop(IPlayer &player, bool enter, TimePoint now);
    // Машина застримлена игроку (VehicleSystem пробрасывает onVehicleStreamIn) —
    // оповещает subscribeStreamedInForPlayer (бизнес переприменяет пер-игроковые
    // params, ядро их сам не восстанавливает).
    void onVehicleStreamIn(IVehicle &vehicle, IPlayer &player);
    void onVehicleCreated(IVehicle &vehicle);
    void onVehicleDestroyed(IVehicle &vehicle);
    void onVehicleRespawn(IVehicle &vehicle); // респаун — HP снова полное
    // Смерть машины (HP -> 0). serverKilled (explode) -> died-наблюдатели
    // (политика бизнеса). Без санкции наблюдатели НЕ зовутся; ПЕРВАЯ смерть
    // бэкофф-окна взводит queueRespawn (VehicleSystem откладывает respawnIfDead
    // — возврат целой НА МЕСТЕ, точка смерти фиксируется здесь), повторная —
    // нет (машину вернёт ядровой death-таймер на spawn-точку). reporter —
    // killer из ядра: сам репорт НЕ флажится (сервер не видит урона по пустой
    // машине, честная клиентская детонация неотличима от фейка), но
    // нечеловеческий ТЕМП репортов одного игрока — vehicleHack.
    Outcome onVehicleDeath(IVehicle &vehicle, IPlayer &reporter);
    // Отложенный возврат несанкционированно убитой машины (таймер-колбэк
    // VehicleSystem, ВНЕ death-диспатча ядра): respawn(), только если машина всё
    // ещё мертва и пуста. Возврат НА МЕСТЕ: SDK не умеет снять deathData без
    // respawn(), поэтому spawn-точка на время respawn() подменяется точкой
    // смерти и затем восстанавливается — репортер смерти не телепортирует чужую
    // машину на её spawn-точку. Ядро само мёртвую так быстро не вернёт: перед
    // проверкой death-таймера оно поднимает lastOccupiedTime до момента смерти
    // (vehicles_impl.hpp onTick) — пустую, в т.ч. никогда не занятую, возвращает
    // именно этот таймер; занятую (читер-пассажир в вреке) вернёт ядровой
    // death-таймер после освобождения. respawn() чистит deathData ядра —
    // второго, ядрового респавна не будет.
    void respawnIfDead(int vehicleId);
    void resetPlayer(int playerId);

  private:
    struct VehicleState
    {
        bool exists = false;
        float health = MAX_HEALTH; // серверное HP
        int driverId = -1;      // обратный индекс «машина -> водитель»
        bool editBypass = false; // машину двигает сервер (редактор) — синк не валидируем
        bool stalled = false;   // заглохла: HP на клампе, двигатель не заводится (снимает repair)
        bool serverKilled = false; // смерть санкционирована сервером (explode); снимают repair/респаун
        bool respawnQueued = false; // respawnIfDead уже назначен (спам setDead не плодит таймеры)
        Vector3 deathPos = {0.0f, 0.0f, 0.0f}; // где сервер зафиксировал несанкционированную смерть
        float deathAngle = 0.0f;               // угол на момент смерти (spawn-подмена respawnIfDead)
        TimePoint lastUnsanctionedDeath; // бэкофф: повторная смерть в окне уходит ядровому death-таймеру
        // Уведомление subscribeUnsanctionedDeath уже отправлено для ТЕКУЩЕЙ мёртвой
        // фазы машины (одна волна спама setDead -> один колбэк, а не по таймеру):
        // взводится в onVehicleDeath на первом же вызове волны, снимается respawn'ом
        // (onVehicleRespawn) — машина ожила, следующая смерть уже НОВАЯ волна.
        bool unsanctionedNotified = false;
        Owner owner = Owner::None; // серверный тег владельца
        int ownerId = -1;          // id владельца в рамках типа (None — -1)
        float fuel = FUEL_CAPACITY; // топливо в баке
        bool outOfFuel = false;     // пустой бак: двигатель не заводится (снимает refuel)
        TimePoint lastChange;    // грейс после серверного изменения (кламп его НЕ освежает)
        TimePoint lastClampSend; // темп повторных кламп-RPC (stallIfCritical/douse-гейт)
        TimePoint lastFlag;      // rate limit нарушений

        // Трек непрерывного пребывания МАШИНЫ в известной ремзоне
        // (MOD_SHOPS/PAY_N_SPRAY объединённо) — обновляется ТОЛЬКО из
        // verifyHealth на каждом принятом driver-sync (не из самой
        // vehicle.getPosition(), которую ядро пишет безусловно из синка без
        // валидации скорости достижения; трек ведётся, пока за рулём КТО-ТО
        // есть, независимо от того, кто именно — привязан к машине). Зовётся
        // только для occupant.seat==0, т.е. трек стоит, пока водителя нет
        // (машина без водителя не может подать SCM-событие, dwell ей не нужен).
        // zoneEnteredAt — момент, когда машина ПОСЛЕДНИЙ РАЗ вошла в зону
        // НЕПРЕРЫВНО (сбрасывается выходом из зоны на любом апдейте, а также
        // явно на respawn — телепорт, не физический подъезд); inZoneNow — кэш
        // «в зоне на последнем апдейте» для детекта входа/выхода без лишнего
        // nearAnyZone. TimePoint{} — «трек ещё не видел машину в зоне».
        // Не персистится — одноразовая жизнь машины/сервера.
        TimePoint zoneEnteredAt;
        bool inZoneNow = false;
    };

    struct Occupant
    {
        int vehicleId = -1;
        int seat = -1;
        // Сессия мод-шопа, подтверждённая СЕРВЕРОМ: взводится только входом с
        // машиной у ворот шопа (onModShop + RepairZones); клиентскому
        // isInModShop ядра не верим — его ставит тот же SCM-пакет. Гейт для
        // validateMod/validatePaintJob/validateRespray и санкций ремонта —
        // каждый из них ВСЕГДА перепроверяет зону по ТЕКУЩЕЙ позиции машины
        // поверх этого флага, так что «карманная» сессия (клиент подавил exit)
        // без физического нахождения в шопе ничего не даёт. Сбрасывают выход
        // из шопа (в т.ч. отклонённый вне зоны), смена привязки (bindOccupant)
        // и resetPlayer.
        bool inModShop = false;
    };

    // Темп репортов смертей машин от одного игрока (скользящее окно): сам репорт
    // не нарушение, нечеловеческий темп флажится (см. onVehicleDeath).
    struct DeathReportRate
    {
        TimePoint windowStart;
        int count = 0;
        TimePoint lastFlag; // rate limit записи нарушения
    };

    // Санкция «машину починил легальный сервис» (мод-шоп, Pay'n'Spray) —
    // единственный рост серверного HP по клиентскому событию; каждый вызов
    // обязан стоять за серверным гейтом локации (RepairZones / inModShop).
    void sanctionRepair(int vehicleId, TimePoint now);
    bool isDriverOf(int playerId, const IVehicle &vehicle) const;
    // HP у порога — кламп над пожаром + глушим (вызывать после падения HP).
    // Повтор кламп-RPC у уже заглохшей темпуется lastClampSend (раз в SYNC_GRACE);
    // lastChange (грейс repair-детектора) кламп НЕ освежает. Повтор belowClamp —
    // не нарушение (перевёрнутая машина честно даёт его минутами).
    void stallIfCritical(IVehicle &vehicle, VehicleState &st, TimePoint timeNow);
    // Тушение машины БЕЗ водителя по ядровому HP: ниже порога — кламп + глушим,
    // как stallIfCritical у водителя. Ядровое HP без водителя опускает только
    // принятый unoccupied-синк ПАССАЖИРА (SeatID != 0) — у по-настоящему пустой
    // оно ниже клампа не бывает (вызов — no-op). Мёртвую (isDead) и serverKilled
    // НЕ трогает — кламп воскресил бы HP до детонации explode(). NaN/Inf из
    // синка считается добитой (кламп перетирает мусор). Зовётся из secondTick и
    // с accept-пути validateUnoccupied.
    void douseUnoccupiedFire(IVehicle &vehicle, VehicleState &st, TimePoint timeNow);
    // Снять «заглохла» (ремонт/респаун): вернуть двигателю клиентский авто-режим.
    void clearStall(IVehicle &vehicle, VehicleState &st);
    // Обновляет zone dwell-трек по ТЕКУЩЕЙ позиции машины: зовётся ТОЛЬКО из
    // verifyHealth на каждом принятом driver-sync. Вход в зону (inZoneNow
    // false->true) фиксирует zoneEnteredAt = timeNow; выход (true->false) стирает
    // его (TimePoint{}); внутри зоны без выхода — не трогает (иначе непрерывность
    // рвалась бы каждым апдейтом). Дешёвая проверка O(зон), без аллокаций — та же
    // цена, что уже была у гейтов SCM, просто теперь ещё и на driver-sync.
    void trackZoneDwell(VehicleState &st, Vector3 position, TimePoint timeNow);
    // Машина СЕЙЧАС в зоне И непрерывно простояла в ней не меньше ZONE_DWELL_MIN
    // (см. trackZoneDwell/verifyHealth). Заменяет голое nearAnyZone во ВСЕХ
    // SCM-гейтах ремонта — мгновенная сверка позиции доверяла бы клиент-
    // авторитетному vehicle.getPosition() без истории.
    bool dwelledInZone(const VehicleState &st, std::span<const RepairZones::Zone> zones, Vector3 position,
                       TimePoint timeNow) const;

    IVehiclesComponent *m_vehicles = nullptr;
    PlayerLocationService *m_location = nullptr;
    GridService *m_grid = nullptr; // пространственный индекс машин (anyVehicleNear)

    // Оповещает наблюдателей смены позиции (грид) о принятой позиции машины.
    void notifyMoved(IVehicle &vehicle, Vector3 acceptedPosition);

    std::vector<VehicleObserver> m_createdObservers;
    std::vector<VehicleObserver> m_destroyedObservers;
    std::vector<VehicleObserver> m_diedObservers;
    std::vector<VehicleObserver> m_respawnedObservers;
    std::vector<VehicleMoveObserver> m_movedObservers;
    std::vector<DriverGateObserver> m_driverGateObservers;
    std::vector<UnsanctionedDeathObserver> m_unsanctionedDeathObservers;
    std::vector<FuelEmptyObserver> m_fuelEmptyObservers;
    std::vector<EngineBrokenObserver> m_engineBrokenObservers;
    std::vector<StreamedInForPlayerObserver> m_streamedInForPlayerObservers;
    std::vector<TunedObserver> m_tunedObservers;

    std::array<VehicleState, VEHICLE_POOL_SIZE> m_vehicleState;
    std::array<Occupant, MAX_PLAYERS> m_occupants;
    std::array<DeathReportRate, MAX_PLAYERS> m_deathReports;
};
