#pragma once

#include "Macro.h"
#include "Services/IService.h"
#include "player.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <string>

class AudioSystem;
struct ICore;

// Сервис звука: игровые звуки SA по ID и аудиопотоки (интернет-радио, музыка).
//
//   m_audio.playSound(player, 1057);                    // звук события (UI, чекпоинт)
//   m_audio.playSoundAt(player, 1159, pos);             // позиционный звук
//   m_audio.playSoundNear(pos, 30.0f, 1159);            // всем в радиусе
//
//   m_audio.playStream(player, "http://radio.example/stream"); // музыка на игрока
//   m_audio.playStreamAt(player, url, pos, 30.0f);      // колонка/бумбокс (затухает)
//   m_audio.stopStream(player);
//   m_audio.currentStream(playerId);                    // что сейчас играет ("" — ничего)
//
// Валидация: URL потока может приходить от игрока (радио в машине, бумбокс) —
// проверяются длина (буфер клиента), печатный ASCII без пробелов и схема
// http(s). Звуки/потоки — данные сервер -> клиент, клиентского ввода у самого
// сервиса нет.
class AudioService final : public IService
{
    friend AudioSystem;

  public:
    static constexpr std::size_t MAX_URL_LENGTH = 255; // буфер URL у клиента

    // Проверка URL потока (для пользовательских URL). error — utf-8.
    static bool validateStreamUrl(StringView url, std::string &error);

    // --- звуки SA ---
    void playSound(IPlayer &player, std::uint32_t soundId); // без позиции (на полную громкость)
    void playSoundAt(IPlayer &player, std::uint32_t soundId, const Vector3 &position);
    void playSoundNear(const Vector3 &position, float radius, std::uint32_t soundId);

    // --- аудиопотоки ---
    // false — URL не прошёл валидацию (поток не запускался).
    bool playStream(IPlayer &player, StringView url);
    bool playStreamAt(IPlayer &player, StringView url, const Vector3 &position, float distance);
    void stopStream(IPlayer &player);
    bool isStreaming(int playerId) const;
    const std::string &currentStream(int playerId) const; // "" — ничего не играет

  private:
    // Вызываются AudioSystem.
    void initialize(ICore *core);
    void resetPlayer(int playerId);

    static std::uint32_t clampSoundId(std::uint32_t soundId);

    ICore *m_core = nullptr;
    std::array<std::string, MAX_PLAYERS> m_streams; // текущий URL потока per-player
};
