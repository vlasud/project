#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Декларативные права команды. Команда объявляет ГРУППУ и ПОРОГ, не зная, КАК
// они проверяются — резолвер (его задаёт владелец сервиса) транслирует Spec в
// факты сервисов. Так одна механика гейтит и админов (числовой уровень 1..5,
// AdminLevel), и орг-команды (членство/битовая маска фракции — это НЕ числовая
// лестница, поэтому отдельные ветки). Тегированный union: значение читается по
// kind.
struct PermissionSpec
{
    enum class Kind : std::uint8_t
    {
        None,             // без проверки
        AdminLevel,       // мин. админ-уровень
        FactionMember,    // членство в конкретной фракции
        FactionPermission // членство + биты доступа фракции
    };
    Kind kind = Kind::None;
    int adminLevel = 0;            // Kind::AdminLevel: мин. уровень 1..5
    int factionId = 0;             // Faction*: какая фракция
    std::uint64_t factionMask = 0; // FactionPermission: нужные биты (0 = только членство)
    // Инвариант фабрик фракций: id — реальный id фракции (> 0). factionId == 0 это
    // NO_FACTION, и резолвер пропустил бы любого не-члена — такой спек запрещён.
    static PermissionSpec admin(int level)
    {
        return {Kind::AdminLevel, level, 0, 0};
    }
    static PermissionSpec factionMember(int id)
    {
        return {Kind::FactionMember, 0, id, 0};
    }
    static PermissionSpec faction(int id, std::uint64_t mask)
    {
        return {Kind::FactionPermission, 0, id, mask};
    }
};
using PermissionResolver = std::function<bool(IPlayer &, const PermissionSpec &)>;

// Реестр команд чата. Другие системы регистрируют команды через add(), указывая
// тип каждого параметра (целое число или строка). Сервис сам:
//   * проверяет, что переданы все параметры (иначе шлёт сигнатуру команды);
//   * проверяет и парсит числовые параметры (иначе шлёт сообщение об ошибке);
//   * отдаёт обработчику уже готовые типизированные значения через CommandArgs.
//
// Последний параметр всегда «жадный»: забирает весь оставшийся текст, включая
// пробелы (удобно для /n [текст], /sms [id] [текст]).
//
// Горячий путь dispatch() не выделяет память под разбор: имя и аргументы — это
// StringView-срезы исходного буфера. Поиск команды — O(1) через unordered_map с
// гетерогенным ключом. Единственная аллокация — вектор аргументов. Срезы валидны
// на время вызова обработчика; хранить их за его пределами нельзя.
//
// Корневой антифлуд: dispatch() блокирует исполнение ВСЕХ команд игрока, если он
// шлёт одну и ту же команду подряд слишком часто (порог CMD_FLOOD_THRESHOLD
// повторов в окне CMD_FLOOD_WINDOW). Состояние — пер-игрок в самом сервисе;
// чистится reset() на дисконнекте (иначе блок/счётчик утечёт в переиспользованный
// слот). Проверка стоит до разбора аргументов — ловит и спам usage-сообщений.
class PlayerCommandService final : public IService
{
  public:
    struct Param
    {
        enum Type
        {
            Int,   // целое число; сервис проверит и распарсит
            String // произвольный текст
        };

        Type type = String;
        std::string name; // отображаемое имя, например "id игрока" или "текст"
    };

  private:
    struct Arg
    {
        StringView text; // исходный токен (для последнего параметра — жадный остаток)
        int number = 0;  // распарсенное значение, если параметр объявлен как Int
    };

  public:
    // Типобезопасный доступ к разобранным аргументам. Сервис уже проверил типы
    // при разборе, поэтому геттеры безопасны: getInt() для Int-параметра вернёт
    // готовое число, getString() — исходный текст любого параметра.
    class CommandArgs
    {
      public:
        explicit CommandArgs(const std::vector<Arg> &args) : m_args(args)
        {
        }

        int getInt(std::size_t index) const
        {
            return m_args[index].number;
        }
        StringView getString(std::size_t index) const
        {
            return m_args[index].text;
        }
        std::size_t count() const
        {
            return m_args.size();
        }

      private:
        const std::vector<Arg> &m_args;
    };

    using Handler = std::function<void(IPlayer &, const CommandArgs &)>;

    // Категория команды для /help: задаёт секцию справки и попадание в неё.
    // Hidden — НЕ показывать в /help (админ/dev/модерация — для них /ahelp).
    // Фильтрацию фракционной секции по членству делает вызывающий /help (сервису
    // команд про фракции знать не нужно).
    enum class HelpCategory : std::uint8_t
    {
        Hidden,
        ChatRP,
        Economy,
        Faction,
        Misc
    };

    // Доступная игроку админ-команда: имя (нижний регистр, как хранится), её
    // порог по уровню и описание (utf-8). Используется /ahelp для построения
    // списка тем же резолвером, что и dispatch.
    struct AccessibleCommand
    {
        std::string name;
        int adminLevel;
        std::string description; // utf-8; кодируется один раз вместе с телом диалога
    };

    // Перечислить АДМИН-команды (Kind::AdminLevel), доступные игроку прямо сейчас
    // по его эффективному уровню — фильтр тем же резолвером, что и dispatch. Так
    // список не дрейфует от набора зарегистрированных команд. Игровые/None/Faction
    // не включаются. Редкий путь (по команде): O(числа команд) + аллокация вектора.
    std::vector<AccessibleCommand> collectAccessible(IPlayer &player) const;

    // Команда для /help: имя (нижний регистр, как хранится), описание (utf-8) и
    // категория. Фильтрацию по фракции делает вызывающий (/help).
    struct HelpCommand
    {
        std::string name;
        std::string description; // utf-8
        HelpCategory category;
    };

    // Перечислить ВСЕ команды с helpCategory != Hidden (для /help). Категория и
    // описание берутся как заявлены при регистрации; фильтр по правам тут НЕ
    // применяется (фракционные команды — Kind::None, право проверяется внутри
    // обработчика). Холодный путь (по команде): O(числа команд) + аллокация вектора.
    std::vector<HelpCommand> collectHelpCommands() const;

    // Зарегистрировать команду. name — без '/'. Вызывается из конструктора системы.
    // perm — порог прав (по умолчанию None — открыта всем). Проверяется в
    // dispatch заданным резолвером. description — utf-8 (НЕ cp1251): собирается в
    // тело диалога /help|/ahelp и кодируется один раз вместе со всем телом.
    // helpCategory — секция /help (Hidden — в /help не показывать).
    void add(std::string name, std::vector<Param> params, Handler handler, PermissionSpec perm = {},
             std::string description = {}, HelpCategory helpCategory = HelpCategory::Hidden);

    // Резолвер прав — общий для всех команд, задаёт владелец сервиса
    // (PlayerCommandSystem). Транслирует PermissionSpec в факты сервисов
    // (AdminService/FactionService). До установки — все perm != None считаются
    // НЕдоступными (см. dispatch).
    void setPermissionResolver(PermissionResolver resolver);

    // Разобрать сообщение и вызвать обработчик. Возвращает true, если команда найдена.
    bool dispatch(IPlayer &player, StringView message);

    // Сброс пер-игрокового антифлуд-состояния. Зовётся системой на дисконнекте.
    void reset(int playerId);

  private:
    struct ParamInfo
    {
        Param::Type type;
        std::string numberError; // готовое сообщение об ошибке для Int (пусто для String)
    };

    struct Command
    {
        std::vector<ParamInfo> params; // описания параметров; размер = их число
        std::string usage;             // готовая строка-подсказка, уже в cp1251
        Handler handler;
        PermissionSpec perm;                            // порог прав; None — открыта всем
        std::string description;                        // utf-8; для /help|/ahelp
        HelpCategory helpCategory = HelpCategory::Hidden; // секция /help (Hidden — не показывать)
    };

    // Транспарентные хэш/сравнение по нижнему ASCII-регистру. Ключи хранятся уже
    // приведёнными к нижнему регистру, поэтому смешанный регистр на входе находит
    // ту же запись, не выделяя строку под ключ поиска.
    struct CiHash
    {
        using is_transparent = void;
        std::size_t operator()(StringView s) const noexcept;
    };
    struct CiEqual
    {
        using is_transparent = void;
        bool operator()(StringView a, StringView b) const noexcept;
    };

    // Пер-игроковое антифлуд-состояние. lastCommand — фиксированный буфер (без
    // аллокаций в горячем пути): длинные команды сравниваются по префиксу длины
    // LAST_COMMAND_CAP — для детекта одинакового спама этого достаточно.
    static constexpr std::size_t LAST_COMMAND_CAP = 128;
    struct State
    {
        std::array<char, LAST_COMMAND_CAP> lastCommand{}; // нормализованная (ASCII-lower) строка после '/'
        std::size_t lastCommandLen = 0;
        int repeatCount = 0;     // подряд идущих одинаковых вводов в окне
        TimePoint lastCommandAt; // момент последнего ввода (для окна повторов)
        TimePoint blockUntil;    // до этого момента все команды игрока заблокированы
        TimePoint lastNoticeAt;  // последний показ сообщения о блоке (троттлинг)
    };

    // Корневой антифлуд: true — команду исполнять НЕ нужно (молча/с сообщением
    // отклонена). normalizedLine — строка после '/' в нижнем ASCII-регистре.
    bool isFlooding(IPlayer &player, StringView normalizedLine, TimePoint now);

    std::unordered_map<std::string, Command, CiHash, CiEqual> m_commands;
    std::array<State, MAX_PLAYERS> m_state;
    PermissionResolver m_permissionResolver;
};
