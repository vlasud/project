#pragma once

#include <string>

class Player
{
  public:
    static void *operator new(size_t size) = delete;
    static void operator delete(void *ptr) = delete;
    static void *operator new[](size_t size) = delete;
    static void operator delete[](void *ptr) = delete;

    void setName(const std::string &name);
    const std::string &getName() const;

    void setMoney(unsigned long long money);
    unsigned long long getMoney() const;

    void setSkin(unsigned skin);
    unsigned getSkin() const;

    void setIsLoggedIn(bool isLoggedIn);
    bool getIsLoggedIn() const;

  private:
    std::string m_name;
    unsigned long long m_money = 0l;
    unsigned m_skin = 1;
    bool m_isLoggedIn = false;
};
