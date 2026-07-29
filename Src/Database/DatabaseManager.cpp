#include "Database/DatabaseManager.h"

#include "Log/LogManager.h"
#include "ThreadPool/ThreadPool.h"
#include "core.hpp"
#include "mysqlx/devapi/settings.h"
#include "mysqlx/xdevapi.h"
#include <cassert>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
const std::string HOST = "127.0.0.1";
const std::string USER = "root";
const std::string PASSWORD = "root";
const std::string DATABASE = "test";
const std::string PORT = "33060";
} // namespace

bool SessionWrapper::initialize()
{
    std::string uri = "mysqlx://" + USER + ":" + PASSWORD + "@" + HOST + ":" + PORT + "/" + DATABASE;
    try
    {
        m_session = std::make_unique<mysqlx::Session>(uri);
        return true;
    }
    catch (const std::exception &e)
    {
        LogManager::log(Error, std::string("Failed to open MySQL session: ") + e.what());
        return false;
    }
}

mysqlx::Schema SessionWrapper::getSchema()
{
    if (!m_session)
    {
        // Слот с провалившимся initialize() не должен доходить до сюда (StaticPool
        // его больше не выдаёт), но если всё же дошёл — это исключение поймает
        // catch в ThreadPool и уведёт в errorCallback, а не UB на нулевом unique_ptr.
        throw std::runtime_error("SessionWrapper::getSchema: session was not initialized");
    }
    return m_session->getSchema(DATABASE);
}

void DatabaseManager::initialize()
{
    // initialize() вызывается из onLoad на главном потоке — фиксируем его id,
    // чтобы потом assertMainThread() ловил доступ к пулу/очередям с воркеров.
    s_mainThreadId = std::this_thread::get_id();

    // Слоты с проваленным initialize() (обрыв к MySQL) исключаются из пула
    // насовсем — get() их больше не выдаст (см. StaticPool::init).
    size_t opened = m_sessionPool.init([](SessionWrapper &wrapper) { return wrapper.initialize(); });

    if (opened == 0)
    {
        LogManager::log(Error, "DatabaseManager: no MySQL sessions could be opened. Check that MySQL X Protocol "
                               "(port 33060) is reachable and credentials are correct.");
        return;
    }

    if (opened < m_sessionPool.size())
    {
        LogManager::log(Error, "DatabaseManager: only " + std::to_string(opened) + "/" +
                                   std::to_string(m_sessionPool.size()) +
                                   " MySQL sessions could be opened; failed slots excluded from the pool.");
    }

    LogManager::log(Message, "DatabaseManager initialized with " + std::to_string(opened) + " sessions");
}

// Проверка инварианта «доступ к пулу/очередям только с главного потока».
// До initialize() id ещё не зафиксирован — тогда не проверяем.
void DatabaseManager::assertMainThread()
{
    assert((s_mainThreadId == std::thread::id{} || std::this_thread::get_id() == s_mainThreadId) &&
           "DatabaseManager: pool/queue access must happen on the main thread");
}

// Сессия возвращается в пул и запускается следующий отложенный запрос.
// Вызывается на главном потоке и при успехе, и при ошибке — поэтому пул
// не утекает, даже если запросы падают подряд.
void DatabaseManager::releaseAndPump(SessionWrapper *sessionWrapper)
{
    assertMainThread();
    m_sessionPool.release(sessionWrapper);

    if (!m_queue.empty())
    {
        PendingThrow pending = std::move(m_queue.front());
        m_queue.pop();
        // Именно dispatchThrow, а не throwQuery: ключ этой задачи занят ЕЮ ЖЕ с
        // момента постановки в бэкпрешер, и повторный заход через submitThrow увидел
        // бы его занятым — задача встала бы в очередь ожидания самой себя навсегда.
        // Гард берёт ключ move'ом: если dispatchThrow бросит, ключ отпустится.
        KeyGuard keyGuard(std::move(pending.key));
        dispatchThrow(keyGuard, std::move(pending.task), std::move(pending.errorCallback));
    }
    else if (!m_selectQueue.empty())
    {
        PendingSelect pending = std::move(m_selectQueue.front());
        m_selectQueue.pop();
        pending.retry(); // повторно вызовет selectQuery<T> — сессия уже свободна
    }
}

// Not thread safe, should be called from the main thread
void DatabaseManager::throwQuery(DatabaseManager::Task task, DatabaseManager::ErrorCallback errorCallback)
{
    submitThrow({}, std::move(task), std::move(errorCallback));
}

void DatabaseManager::throwQueryOrdered(std::string orderingKey, DatabaseManager::Task task,
                                        DatabaseManager::ErrorCallback errorCallback)
{
    submitThrow(std::move(orderingKey), std::move(task), std::move(errorCallback));
}

void DatabaseManager::submitThrow(std::string key, DatabaseManager::Task task,
                                  DatabaseManager::ErrorCallback errorCallback)
{
    assertMainThread();
    if (!key.empty() && !m_busyKeys.insert(key).second)
    {
        // По ключу уже что-то выполняется — встаём за ним. Свою сессию НЕ занимаем:
        // иначе ждущие задачи выели бы пул и заблокировали независимые запросы.
        // Очередь берём отдельной строкой: key уходит в PendingThrow move'ом, и
        // выражение вида m_keyQueue[key].push(... std::move(key) ...) читалось бы как
        // зависящее от порядка вычисления.
        std::queue<PendingThrow> &waiting = m_keyQueue[key];
        waiting.push(PendingThrow{std::move(task), std::move(errorCallback), key});
        if (waiting.size() == KEY_QUEUE_WARN_DEPTH)
        {
            LogManager::log(Warning, "DatabaseManager: ordered queue for key '" + key +
                                         "' reached " + std::to_string(KEY_QUEUE_WARN_DEPTH) +
                                         " entries; the key looks stuck and this row is no longer being written");
        }
        return;
    }
    // Ключ (если он есть) занят НАМИ — и сразу уходит во владение гарду. Между
    // insert выше и этой строкой нет ни одной операции, способной бросить, поэтому
    // «помечен занятым, но никем не освобождается» невозможно по построению.
    KeyGuard keyGuard(std::move(key));
    dispatchThrow(keyGuard, std::move(task), std::move(errorCallback));
}

void DatabaseManager::dispatchThrow(KeyGuard &keyGuard, DatabaseManager::Task task,
                                    DatabaseManager::ErrorCallback errorCallback)
{
    assertMainThread();
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        // Ключ остаётся занятым, пока задача лежит в бэкпрешере: иначе следующая
        // задача того же ключа обогнала бы её и порядок сломался бы именно там, где
        // его и просили. commit — ТОЛЬКО после успешного push: если он бросит, ключ
        // отпустит гард вызывающего, а не запрёт навсегда.
        m_queue.push(PendingThrow{std::move(task), std::move(errorCallback), keyGuard.key()});
        keyGuard.commit();
        return;
    }

    // Гард сессии вернёт её в пул, если addTask бросит до постановки задачи; ключ на
    // этом пути держит гард вызывающего.
    SessionGuard guard(sessionWrapper);

    ThreadPool::Task<bool> asyncTask;

    asyncTask.func = [task = std::move(task), sessionWrapper]()
    {
        task(sessionWrapper->getSchema());
        return true;
    };

    // Оба гарда — RAII и в успехе тоже: бросок из releaseAndPump иначе не дал бы
    // освободить ключ, и записи по этой строке встали бы навсегда. Порядок разрушения
    // обратный объявлению — сперва возвращается сессия, потом освобождается ключ, так
    // что следующая задача ключа сразу получает освободившийся слот.
    asyncTask.callback = [sessionWrapper, key = keyGuard.key()](bool)
    {
        KeyRelease keyRelease(key);
        PumpGuard release(sessionWrapper);
    };

    asyncTask.errorCallback = [errorCallback = std::move(errorCallback), sessionWrapper,
                               key = keyGuard.key()](const std::string &error)
    {
        KeyRelease keyRelease(key);
        PumpGuard release(sessionWrapper); // релиз даже если обработчик ошибки бросит
        LogManager::log(Error, "DatabaseManager: query failed: " + error);
        if (errorCallback)
        {
            errorCallback(error);
        }
    };

    ThreadPool::addTask(std::move(asyncTask));
    guard.commit();    // задача принята — релиз теперь делает releaseAndPump в её колбэке
    keyGuard.commit(); // ключ теперь отпустит KeyGuard внутри колбэка задачи
}

// Задача ключа отработала. Если по нему ждут — запускаем следующую, НЕ снимая
// занятость (её тут же перехватывает эта следующая задача). Если очередь пуста —
// стираем обе записи, чтобы карты не росли от числа обслуженных ключей.
void DatabaseManager::releaseKey(const std::string &key)
{
    if (key.empty())
    {
        return;
    }
    assertMainThread();

    const auto queued = m_keyQueue.find(key);
    if (queued == m_keyQueue.end() || queued->second.empty())
    {
        if (queued != m_keyQueue.end())
        {
            m_keyQueue.erase(queued);
        }
        m_busyKeys.erase(key);
        return;
    }

    PendingThrow next = std::move(queued->second.front());
    queued->second.pop();
    // Ключ не снимаем — его перехватывает эта следующая задача, и владеет им гард:
    // бросок из dispatchThrow отпустит ключ, а не оставит очередь запертой.
    KeyGuard keyGuard(std::move(next.key));
    dispatchThrow(keyGuard, std::move(next.task), std::move(next.errorCallback));
}

// selectQuery<T> / dispatchSelect<T> — шаблонные, определены в DatabaseManager.h.
