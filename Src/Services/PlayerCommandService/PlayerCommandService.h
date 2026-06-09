#pragma once

#include "../IService.h"
#include "player.hpp"
#include "types.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Зарегистрировать команду. name — без '/'. Вызывается из конструктора системы.
    void add(std::string name, std::vector<Param> params, Handler handler);

    // Разобрать сообщение и вызвать обработчик. Возвращает true, если команда найдена.
    bool dispatch(IPlayer &player, StringView message);

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

    std::unordered_map<std::string, Command, CiHash, CiEqual> m_commands;
};
