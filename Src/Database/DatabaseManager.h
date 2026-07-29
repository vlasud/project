#pragma once

#include "Log/LogManager.h"
#include "Pools/StaticPool.h"
#include "ThreadPool/ThreadPool.h"
#include <functional>
#include <mysqlx/xdevapi.h>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

class SessionWrapper
{
  public:
    bool initialize();
    mysqlx::Schema getSchema();

  private:
    std::unique_ptr<mysqlx::Session> m_session;
};

// Асинхронный доступ к MySQL поверх ThreadPool.
//
// КОНТРАКТ ПОТОКОВ. Connector/C++ (X DevAPI) требует, чтобы Session и любой
// Result использовались максимум ОДНИМ потоком за раз — объект нельзя
// конструировать на одном потоке, а читать/разрушать на другом. Поэтому здесь
// НИ ОДИН mysqlx-объект не пересекает границу потока:
//
//   • throwQuery  — задача целиком выполняется на воркере, ничего не возвращает.
//   • selectQuery — задача выполняется И ПОЛНОСТЬЮ ВЫЧИТЫВАЕТ результат на
//                   воркере, возвращая владеющие данные T (контейнеры/POD).
//                   На главный поток в callback приходит уже готовое T.
//
// Раньше selectQuery отдавал на главный поток живой RowResult и дренажил его там
// — это нарушало контракт и роняло сервер порчей общей кучи (ucrtbase). Не
// возвращайте RowResult/SqlResult/Row из задачи: вычитывайте прямо в ней.
//
// КОНТРАКТ ПОРЯДКА. По умолчанию порядка НЕТ: две задачи уходят разным воркерам и
// коммитятся как получится. Для независимых строк это и не нужно, но пара «DELETE
// строки + следующий за ним INSERT той же строки» при перестановке оставляет в БД
// удалённое состояние, хотя в памяти запись живая — то есть тихо теряет то, за что
// уже списаны деньги. Такие последовательности идут через throwQueryOrdered с общим
// ключом; всё остальное — обычным throwQuery и параллельно.
class DatabaseManager
{
    using Task = std::function<void(mysqlx::Schema)>;
    using ErrorCallback = std::function<void(const std::string &)>;

  public:
    static void initialize();

    // errorCallback (опционально) вызывается на главном потоке, если запрос
    // бросил исключение. Сессия в любом случае возвращается в пул, ошибка
    // логируется.
    //
    // ПОРЯДКА МЕЖДУ ДВУМЯ throwQuery НЕТ: задачи разбирает многопоточный пул, и
    // коммиты могут лечь в любом порядке. Для независимых записей это безразлично,
    // но две операции по ОДНОЙ строке (напр. DELETE ставки и следующий за ним
    // INSERT той же ставки) так переставляются и теряют последнюю — для них берите
    // throwQueryOrdered.
    static void throwQuery(Task task, ErrorCallback errorCallback = {});

    // То же, но с ГАРАНТИЕЙ ПОРЯДКА среди задач с одинаковым orderingKey: следующая
    // стартует только после того, как отработал главнопоточный колбэк предыдущей.
    // Задачи с разными ключами (и все безключевые) по-прежнему идут параллельно —
    // сериализуется ровно то, что обязано.
    //
    // Ключ выбирает вызывающий, и он задаёт ЕДИНИЦУ УПОРЯДОЧИВАНИЯ: под одним ключом
    // должны ходить ВСЕ записи, способные затронуть одни и те же строки. Для лота
    // аукциона это лот целиком (снятие лота стирает и его ставки), а не отдельная
    // ставка. Слишком мелкий ключ порядок не спасёт, слишком крупный — просто
    // выстроит в очередь лишнее.
    //
    // Пустой ключ = обычный throwQuery. Порядок с selectQuery НЕ упорядочивается:
    // чтения в проекте идут на старте и на входе игрока, до записей по тем же строкам.
    static void throwQueryOrdered(std::string orderingKey, Task task, ErrorCallback errorCallback = {});

    // task — на воркере: выполняет запрос и вычитывает результат в T.
    // callback — на главном потоке: получает готовое T.
    template <typename T>
    static void selectQuery(std::function<T(mysqlx::Schema)> task, std::function<void(T)> callback,
                            ErrorCallback errorCallback = {});

  private:
    struct PendingThrow
    {
        Task task;
        ErrorCallback errorCallback;
        // Ключ упорядочивания; пуст — задача ничем не связана. Если задача попала в
        // очередь бэкпрешера, её ключ УЖЕ помечен занятым: следующие задачи того же
        // ключа встанут за ней, а не обгонят её.
        std::string key;
    };
    // Бэкпрешер для selectQuery: тип T стёрт — храним замыкание, повторно
    // вызывающее selectQuery<T>, когда освободится сессия.
    struct PendingSelect
    {
        std::function<void()> retry;
    };

    // RAII для занятой сессии: возвращает её в пул при разрушении, если задачу
    // не удалось поставить в очередь (например, ThreadPool::addTask бросил
    // bad_alloc). commit() передаёт владение колбэкам задачи, и тогда релиз
    // делает releaseAndPump. Гарантия: сессия освобождается ровно один раз на
    // ЛЮБОМ пути, утечки слота пула нет.
    class SessionGuard
    {
      public:
        explicit SessionGuard(SessionWrapper *session) : m_session(session)
        {
        }
        SessionGuard(const SessionGuard &) = delete;
        SessionGuard &operator=(const SessionGuard &) = delete;
        ~SessionGuard()
        {
            if (m_session)
            {
                m_sessionPool.release(m_session);
            }
        }
        void commit()
        {
            m_session = nullptr;
        }

      private:
        SessionWrapper *m_session;
    };

    // Тот же RAII для КОЛБЭКОВ: релиз сессии обязан произойти на любом выходе,
    // включая исключение из пользовательского кода. ThreadPool::flush такое
    // исключение проглатывает и процесс не падает, поэтому без RAII слот пула
    // утекал бы молча — а после 16 таких случаев пул пуст и ВСЯ персистенция
    // (деньги, аккаунты, владение, торги) встаёт с одной строкой в логе.
    class PumpGuard
    {
      public:
        explicit PumpGuard(SessionWrapper *session) : m_session(session)
        {
        }
        PumpGuard(const PumpGuard &) = delete;
        PumpGuard &operator=(const PumpGuard &) = delete;
        ~PumpGuard()
        {
            // Деструктор не имеет права бросить (может сработать при раскрутке
            // стека): releaseAndPump запускает отложенные задачи и теоретически
            // бросить может, гасим здесь же. Сессия к этому моменту уже отпущена
            // (release — первая строка releaseAndPump), потерять могли только
            // выпущенную из очереди отложенную задачу — поэтому не молча.
            try
            {
                releaseAndPump(m_session);
            }
            catch (...)
            {
                // Лог сам по себе бросить не должен, но в деструкторе на это нельзя
                // полагаться: сообщение — литерал, вложенный catch страхует от terminate.
                try
                {
                    LogManager::log(Error, "DatabaseManager: releaseAndPump threw; a queued query was dropped");
                }
                catch (...)
                {
                }
            }
        }

      private:
        SessionWrapper *m_session;
    };

    // RAII для ЗАНЯТОГО КЛЮЧА. Ключ обязан освободиться на любом выходе, включая
    // исключение из пользовательского кода и провал постановки задачи в пул:
    // заклинивший ключ не роняет сервер, но НАВСЕГДА и молча запирает все последующие
    // записи по этой строке — отладить такое почти нечем. Пустой ключ — no-op.
    //
    // commit() — как у SessionGuard: владение ключом передано колбэкам принятой
    // задачи, дальше отпускать его будет их гард.
    class KeyGuard
    {
      public:
        // Конструировать ТОЛЬКО из rvalue: move std::string не бросает, поэтому между
        // «ключ помечен занятым» и «им владеет гард» не остаётся ни одной операции,
        // способной бросить. Копирующее конструирование запрещено намеренно — на
        // 32-битной сборке bad_alloc здесь запер бы ключ навсегда, а это тихая
        // остановка записи по строке.
        explicit KeyGuard(std::string &&key) noexcept : m_key(std::move(key))
        {
        }
        KeyGuard(const KeyGuard &) = delete;
        KeyGuard &operator=(const KeyGuard &) = delete;
        const std::string &key() const
        {
            return m_key;
        }
        void commit() noexcept
        {
            m_key.clear();
        }
        ~KeyGuard()
        {
            // Как и PumpGuard: деструктор бросить не имеет права (может сработать при
            // раскрутке стека), а releaseKey запускает следующую задачу очереди ключа.
            try
            {
                releaseKey(m_key);
            }
            catch (...)
            {
                try
                {
                    LogManager::log(Error, "DatabaseManager: releaseKey threw; an ordered query was dropped");
                }
                catch (...)
                {
                }
            }
        }

      private:
        std::string m_key;
    };

    // НЕвладеющий вариант — для колбэков задачи, где ключ уже лежит в захвате лямбды
    // и переживёт гард. Отдельный тип, чтобы освобождение не требовало копии строки:
    // копия могла бы бросить bad_alloc ровно в тот момент, когда ключ обязан быть
    // отпущен, и заперла бы запись по строке навсегда.
    class KeyRelease
    {
      public:
        explicit KeyRelease(const std::string &key) noexcept : m_key(key)
        {
        }
        KeyRelease(const KeyRelease &) = delete;
        KeyRelease &operator=(const KeyRelease &) = delete;
        ~KeyRelease()
        {
            try
            {
                releaseKey(m_key);
            }
            catch (...)
            {
                try
                {
                    LogManager::log(Error, "DatabaseManager: releaseKey threw; an ordered query was dropped");
                }
                catch (...)
                {
                }
            }
        }

      private:
        const std::string &m_key;
    };

    template <typename T>
    static void dispatchSelect(SessionWrapper *sessionWrapper, std::function<T(mysqlx::Schema)> task,
                               std::function<void(T)> callback, ErrorCallback errorCallback);

    // Глубина очереди ожидания по ОДНОМУ ключу, после которой пишем в лог. Записи по
    // одной строке редки (несколько ставок на лот, снимок аккаунта раз в автосейв),
    // поэтому такая очередь означает, что ключ не освобождается. Сам по себе залипший
    // ключ сервер не роняет — он ТИХО перестаёт писать эту строку, и весь смысл
    // порога в том, чтобы отказ был виден в логе, а не только в потерянных деньгах.
    static constexpr std::size_t KEY_QUEUE_WARN_DEPTH = 32;

    // Общий вход обоих throwQuery: занимает ключ (если он есть) либо ставит задачу в
    // очередь этого ключа. Заняв ключ, СРАЗУ передаёт его во владение KeyGuard.
    static void submitThrow(std::string key, Task task, ErrorCallback errorCallback);
    // Собственно постановка в пул. Ключ на этот момент уже занят и принадлежит гарду
    // вызывающего: любой бросок здесь отпустит ключ, а не запрёт его. commit у гарда
    // делаем только когда задачу приняла очередь или пул.
    static void dispatchThrow(KeyGuard &keyGuard, Task task, ErrorCallback errorCallback);
    // Снять занятость ключа и запустить следующую его задачу, если она есть.
    static void releaseKey(const std::string &key);

    static void releaseAndPump(SessionWrapper *sessionWrapper);

    // Весь доступ к пулу/очередям — строго с главного потока (пул и std::queue
    // не синхронизированы; на этом держится и контракт коннектора «одна сессия
    // — один поток»). В debug это ловится assert'ом.
    static void assertMainThread();

    inline static StaticPool<SessionWrapper, 16> m_sessionPool;
    inline static std::queue<PendingThrow> m_queue;
    inline static std::queue<PendingSelect> m_selectQueue;
    // Ключи, по которым задача уже в работе (дошла до пула ЛИБО ждёт в m_queue), и
    // очереди ожидающих по каждому. Обе структуры живут только пока по ключу что-то
    // происходит: последнее освобождение стирает запись, поэтому от числа лотов/домов
    // за аптайм они не растут.
    inline static std::unordered_set<std::string> m_busyKeys;
    inline static std::unordered_map<std::string, std::queue<PendingThrow>> m_keyQueue;
    inline static std::thread::id s_mainThreadId; // фиксируется в initialize()
};

// ------------------------------------------------------ шаблонные определения

template <typename T>
void DatabaseManager::selectQuery(std::function<T(mysqlx::Schema)> task, std::function<void(T)> callback,
                                  ErrorCallback errorCallback)
{
    assertMainThread();
    SessionWrapper *sessionWrapper = m_sessionPool.get();
    if (sessionWrapper == nullptr)
    {
        m_selectQueue.push({[task = std::move(task), callback = std::move(callback),
                             errorCallback = std::move(errorCallback)]() mutable
                            {
                                selectQuery<T>(std::move(task), std::move(callback), std::move(errorCallback));
                            }});
        return;
    }
    dispatchSelect<T>(sessionWrapper, std::move(task), std::move(callback), std::move(errorCallback));
}

template <typename T>
void DatabaseManager::dispatchSelect(SessionWrapper *sessionWrapper, std::function<T(mysqlx::Schema)> task,
                                     std::function<void(T)> callback, ErrorCallback errorCallback)
{
    // Сессия уже занята: до успешной постановки задачи владеет ею гард —
    // если addTask бросит, сессия вернётся в пул, а не утечёт.
    SessionGuard guard(sessionWrapper);

    ThreadPool::Task<T> asyncTask;

    // func — на воркере: запрос + вычитка результата в T (mysqlx-объекты живут
    // и умирают здесь же).
    asyncTask.func = [task = std::move(task), sessionWrapper]() { return task(sessionWrapper->getSchema()); };

    // callback — на главном потоке: только готовые данные T.
    asyncTask.callback = [callback = std::move(callback), sessionWrapper](T data)
    {
        PumpGuard release(sessionWrapper); // релиз даже если колбэк бросит
        callback(std::move(data));
    };

    asyncTask.errorCallback = [errorCallback = std::move(errorCallback), sessionWrapper](const std::string &error)
    {
        PumpGuard release(sessionWrapper);
        LogManager::log(Error, "DatabaseManager: select failed: " + error);
        if (errorCallback)
        {
            errorCallback(error);
        }
    };

    ThreadPool::addTask(std::move(asyncTask));
    guard.commit(); // задача принята — релиз теперь делает releaseAndPump в её колбэке
}
