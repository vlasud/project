#include "Player.h"

void Player::setName(const std::string &name)
{
    m_name = name;
}

const std::string &Player::getName() const
{
    return m_name;
}

void Player::setMoney(unsigned long long money)
{
    m_money = money;
}

unsigned long long Player::getMoney() const
{
    return m_money;
}

void Player::setSkin(unsigned skin)
{
    m_skin = skin;
}

unsigned Player::getSkin() const
{
    return m_skin;
}

void Player::setIsLoggedIn(bool isLoggedIn)
{
    m_isLoggedIn = isLoggedIn;
}

bool Player::getIsLoggedIn() const
{
    return m_isLoggedIn;
}
