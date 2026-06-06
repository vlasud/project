#pragma once

#include <array>
#include <functional>
#include <stack>

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

    T *get()
    {
        if (m_availableIndices.empty())
        {
            return nullptr;
        }
        const size_t index = m_availableIndices.top();
        m_availableIndices.pop();
        return &m_pool[index];
    }

    void release(T *object)
    {
        const size_t index = object - m_pool.data();
        if (index >= N)
        {
            return;
        }
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
    std::stack<size_t> m_availableIndices;
};
