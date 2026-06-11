#include "Systems/SystemRegister.h"

#include "Systems/Core/AntiCheatSystem/AntiCheatSystem.h"
#include "Systems/Core/AttachmentSystem/AttachmentSystem.h"
#include "Systems/Core/AudioSystem/AudioSystem.h"
#include "Systems/Core/CameraSystem/CameraSystem.h"
#include "Systems/Core/ChatSystem/ChatSystem.h"
#include "Systems/Core/CheckpointSystem/CheckpointSystem.h"
#include "Systems/Core/ClassSelectionSystem/ClassSelectionSystem.h"
#include "Systems/Core/DebugCameraSystem/DebugCameraSystem.h"
#include "Systems/Core/EditorSystem/EditorSystem.h"
#include "Systems/Core/GameTextSystem/GameTextSystem.h"
#include "Systems/Core/GangZoneEditorSystem/GangZoneEditorSystem.h"
#include "Systems/Core/GangZoneSystem/GangZoneSystem.h"
#include "Systems/Core/GridDebugSystem/GridDebugSystem.h"
#include "Systems/Core/GridSystem/GridSystem.h"
#include "Systems/Core/LocationDebugSystem/LocationDebugSystem.h"
#include "Systems/Core/MapIconSystem/MapIconSystem.h"
#include "Systems/Core/MovingObjectSystem/MovingObjectSystem.h"
#include "Systems/Core/NicknameSystem/NicknameSystem.h"
#include "Systems/Core/ObjectEditSystem/ObjectEditSystem.h"
#include "Systems/Core/PickupSystem/PickupSystem.h"
#include "Systems/Core/PlayerActivitySystem/PlayerActivitySystem.h"
#include "Systems/Core/PlayerAnimationSystem/PlayerAnimationSystem.h"
#include "Systems/PlayerAuthSystem/PlayerAuthSystem.h"
#include "Systems/PlayerSessionSystem/PlayerSessionSystem.h"
#include "Systems/PlayerSpawnSystem/PlayerSpawnSystem.h"
#include "Systems/Core/PlayerCommandSystem/PlayerCommandSystem.h"
#include "Systems/Core/PlayerConnectionVersionSystem/PlayerConnectionVersionSystem.h"
#include "Systems/Core/PlayerDialogSystem/PlayerDialogSystem.h"
#include "Systems/Core/PlayerHealthSystem/PlayerHealthSystem.h"
#include "Systems/Core/PlayerKeySystem/PlayerKeySystem.h"
#include "Systems/Core/PlayerLocationSystem/PlayerLocationSystem.h"
#include "Systems/Core/PlayerMoneySystem/PlayerMoneySystem.h"
#include "Systems/Core/PlayerSkinSystem/PlayerSkinSystem.h"
#include "Systems/Core/PlayerStateSystem/PlayerStateSystem.h"
#include "Systems/Core/PlayerVelocitySystem/PlayerVelocitySystem.h"
#include "Systems/Core/PlayerWeaponSystem/PlayerWeaponSystem.h"
#include "Systems/Core/SpectateSystem/SpectateSystem.h"
#include "Systems/Core/StreamerSystem/StreamerSystem.h"
#include "Systems/Core/TextDrawEditorSystem/TextDrawEditorSystem.h"
#include "Systems/Core/TextDrawSystem/TextDrawSystem.h"
#include "Systems/Core/TextLabelSystem/TextLabelSystem.h"
#include "Systems/Core/TimerSystem/TimerSystem.h"
#include "Systems/Core/VehicleDebugSystem/VehicleDebugSystem.h"
#include "Systems/Core/WeaponDebugSystem/WeaponDebugSystem.h"
#include "Systems/Core/VehicleSystem/VehicleSystem.h"
#include "Systems/Core/WorldSystem/WorldSystem.h"

void SystemRegister::registerSystems(ICore &core, const ServiceRegister &serviceRegister)
{
    // TimerSystem первым: его initialize() передаёт сервису компонент таймеров
    // раньше, чем другие системы смогут ставить таймеры в своих initialize().
    m_systems.push_back(std::make_unique<TimerSystem>(core, serviceRegister));
    // PlayerSessionSystem раньше всех бизнес- и core-систем: конец сессии (и
    // сохранение у подписчиков) должен отстрелить на дисконнекте ДО того, как
    // чужие обработчики начнут чистить состояние игрока.
    m_systems.push_back(std::make_unique<PlayerSessionSystem>(core, serviceRegister));
    // NicknameSystem максимально рано: невалидный ник отсекается на входящем
    // подключении, до того как остальные системы заведут на игрока состояние.
    m_systems.push_back(std::make_unique<NicknameSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerConnectionVersionSystem>(core, serviceRegister));
    // LocationSystem раньше остальных: на том же апдейте все читают уже принятую
    // позицию; VelocitySystem сразу после — производная от свежей позиции.
    m_systems.push_back(std::make_unique<PlayerLocationSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerStateSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerVelocitySystem>(core, serviceRegister));
    // VehicleSystem раньше GridSystem: на смене стейта привязка пассажира уже
    // сделана, грид читает машину игрока из источника правды.
    m_systems.push_back(std::make_unique<VehicleSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GridSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<StreamerSystem>(core, serviceRegister));
    // PickupSystem после StreamerSystem: маршрутизация подбора опирается на
    // пикапы, созданные стримером.
    m_systems.push_back(std::make_unique<PickupSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<TextLabelSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<WorldSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerActivitySystem>(core, serviceRegister));
    // ObjectEditSystem раньше EditorSystem: роутер событий редактирования должен
    // существовать до того, как редактор начнёт сессии.
    m_systems.push_back(std::make_unique<ObjectEditSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<MovingObjectSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerSkinSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AttachmentSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AudioSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GameTextSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<MapIconSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<CheckpointSystem>(core, serviceRegister));
    // m_systems.push_back(std::make_unique<GridDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<LocationDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<VehicleDebugSystem>(core, serviceRegister));
    // WeaponDebugSystem раньше PlayerWeaponSystem: замер темпа (/rof) видит сырой
    // клиентский поток выстрелов до того, как валидатор начнёт дропать.
    m_systems.push_back(std::make_unique<WeaponDebugSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerDialogSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerCommandSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerKeySystem>(core, serviceRegister));
    // TextDrawSystem раньше редактора: роутер кликов должен существовать до того,
    // как редактор начнёт регистрировать обработчики.
    m_systems.push_back(std::make_unique<TextDrawSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<TextDrawEditorSystem>(core, serviceRegister));
    // GangZoneSystem раньше редактора: сервис должен получить компонент до того,
    // как тулза начнёт создавать зоны.
    m_systems.push_back(std::make_unique<GangZoneSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<GangZoneEditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<AntiCheatSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAnimationSystem>(core, serviceRegister));
    // WeaponSystem раньше HealthSystem: фейковый выстрел (оружие без выдачи)
    // отбрасывается до регистрации bullet sync в health — не легализует give-damage.
    m_systems.push_back(std::make_unique<PlayerWeaponSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerHealthSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerMoneySystem>(core, serviceRegister));
    // SpawnSystem раньше AuthSystem: на спавне сперва применяются интерьер/мир
    // точки спавна, затем auth навешивает экипировку.
    m_systems.push_back(std::make_unique<ClassSelectionSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerSpawnSystem>(core, serviceRegister));
    // SpectateSystem после SpawnSystem: возврат из спектейта перекрывает
    // интерьер/мир точки спавна своим сохранённым местом.
    m_systems.push_back(std::make_unique<SpectateSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<PlayerAuthSystem>(core, serviceRegister));
    // CameraSystem раньше тулзы: проигрыватель путей должен быть подключён до
    // того, как /camera начнёт им пользоваться.
    m_systems.push_back(std::make_unique<CameraSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<DebugCameraSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<EditorSystem>(core, serviceRegister));
    m_systems.push_back(std::make_unique<ChatSystemSystem>(core, serviceRegister));
}

void SystemRegister::initializeSystems(IComponentList *components)
{
    for (const auto &system : m_systems)
    {
        system->initialize(components);
    }
}

void SystemRegister::resetSystems()
{
    for (const auto &system : m_systems)
    {
        system->reset();
    }
}
