#pragma once

#include <array>

template <typename T, size_t N> class IndexPool
{
  public:
    IndexPool(const IndexPool &) = delete;
    IndexPool &operator=(const IndexPool &) = delete;

    static T *get(unsigned id)
    {
        if (id >= m_pool.size())
        {
            return nullptr;
        }
        return &m_pool[id];
    }

  protected:
    IndexPool() = default;

  private:
    inline static std::array<T, N> m_pool;
};
