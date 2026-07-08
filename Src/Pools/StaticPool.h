#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <stack>

// Пул фиксированного размера: get() выдаёт указатель на свободный слот,
// release() возвращает его. НЕ потокобезопасен — синхронизацию обеспечивает
// владелец (см. DatabaseManager, где доступ строго с главного потока).
//
// Инвариант «один слот — один владелец одновременно» критичен: если из-за
// двойного release один индекс окажется в стеке дважды, get() выдаст одну
// ячейку двум владельцам. Поэтому release() проверяет, что слот действительно
// был занят и что указатель принадлежит этому пулу.
template <typename T, size_t N> class StaticPool
{
  public:
    StaticPool()
    {
        for (size_t i = 0; i < N; ++i)
        {
            m_availableIndices.push(i);
        }
    }

    StaticPool(const StaticPool &) = delete;
    StaticPool &operator=(const StaticPool &) = delete;

    // Переинициализация пула, когда создание слота может провалиться (напр.
    // соединение к БД оборвано): initFn вызывается для каждого слота, доступным
    // становится ТОЛЬКО тот, где initFn вернул true — сбойный get() больше
    // никогда не выдаст. Заменяет полное наполнение из конструктора. Возвращает
    // число успешно инициализированных слотов.
    size_t init(const std::function<bool(T &)> &initFn)
    {
        std::stack<size_t> empty;
        m_availableIndices.swap(empty);

        size_t succeeded = 0;
        for (size_t i = 0; i < N; ++i)
        {
            if (initFn(m_pool[i]))
            {
                m_availableIndices.push(i);
                ++succeeded;
            }
        }
        return succeeded;
    }

    T *get()
    {
        if (m_availableIndices.empty())
        {
            return nullptr;
        }
        const size_t index = m_availableIndices.top();
        m_availableIndices.pop();
        m_borrowed[index] = true;
        return &m_pool[index];
    }

    void release(T *object)
    {
        // Указатель обязан указывать ровно на начало слота этого пула.
        const std::ptrdiff_t offset = object - m_pool.data();
        assert(object != nullptr && offset >= 0 && static_cast<size_t>(offset) < N &&
               "StaticPool::release: pointer is not from this pool");
        if (object == nullptr || offset < 0 || static_cast<size_t>(offset) >= N)
        {
            return; // в release-сборке не трогаем чужую память
        }

        const size_t index = static_cast<size_t>(offset);
        // Двойной release затёр бы инвариант «один слот — один владелец».
        assert(m_borrowed[index] && "StaticPool::release: slot was not borrowed (double release?)");
        if (!m_borrowed[index])
        {
            return; // в release-сборке двойной release — безопасный no-op
        }

        m_borrowed[index] = false;
        m_availableIndices.push(index);
    }

    size_t size() const
    {
        return N;
    }

    void forEach(std::function<bool(T &)> func)
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (!func(m_pool[i]))
            {
                break;
            }
        }
    }

  private:
    std::array<T, N> m_pool;
    std::array<bool, N> m_borrowed{}; // true — слот выдан get() и ещё не возвращён
    std::stack<size_t> m_availableIndices;
};
