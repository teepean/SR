/*
 * Test program for recompilation of Linux executables: C++ exceptions
 * (call frame information in .eh_frame, exception tables in .gcc_except_table)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

static int destroyed = 0;

struct Tracker {
    int id;
    explicit Tracker(int i) : id(i) {}
    ~Tracker() { destroyed++; std::printf("~Tracker(%d)\n", id); }
};

struct BaseError { virtual ~BaseError() {} virtual const char *name() const { return "BaseError"; } };
struct Mixin { int code = 7; virtual ~Mixin() {} };
struct DerivedError : BaseError, Mixin { const char *name() const override { return "DerivedError"; } };

struct Shape { virtual ~Shape() {} };
struct Circle : Shape {};
struct Square : Shape {};

struct Member {
    explicit Member(bool fail) { if (fail) throw std::runtime_error("member constructor"); }
};

struct Composite {
    Tracker t;
    Member m;
    explicit Composite(bool fail) try : t(100), m(fail) {
    } catch (const std::exception &e) {
        std::printf("function-try-block: %s\n", e.what());
    }
};

__attribute__((noinline)) static void level3(int kind)
{
    Tracker t(3);
    switch (kind) {
        case 0: throw std::invalid_argument("invalid");
        case 1: throw DerivedError();
        case 2: throw 42;
        case 3: throw std::string("string exception");
        case 4: throw &destroyed;
        default: break;
    }
}

__attribute__((noinline)) static void level2(int kind)
{
    Tracker t(2);
    level3(kind);
}

__attribute__((noinline)) static int level1(int kind)
{
    Tracker t(1);
    try {
        level2(kind);
    } catch (const std::logic_error &e) {
        std::printf("level1 caught logic_error (%s), rethrowing\n", e.what());
        throw;
    }
    return 0;
}

__attribute__((noinline)) static void run_kind(int kind)
{
    try {
        level1(kind);
        std::printf("kind %d: no exception\n", kind);
    } catch (const std::invalid_argument &e) {
        std::printf("kind %d: invalid_argument: %s\n", kind, e.what());
    } catch (const Mixin &m) {
        std::printf("kind %d: Mixin code %d (%s)\n", kind, m.code, dynamic_cast<const BaseError &>(m).name());
    } catch (int value) {
        std::printf("kind %d: int %d\n", kind, value);
    } catch (int *pointer) {
        std::printf("kind %d: pointer %s\n", kind, (pointer == &destroyed) ? "ok" : "wrong");
    } catch (...) {
        std::printf("kind %d: unknown exception\n", kind);
    }
}

__attribute__((noinline)) static long nested(int depth)
{
    std::unique_ptr<Tracker> t(new Tracker(1000 + depth));
    if (depth == 0) throw std::out_of_range("bottom");
    try {
        return nested(depth - 1) + 1;
    } catch (const std::out_of_range &e) {
        if (depth % 2 == 0) {
            std::printf("nested %d: rethrow as overflow\n", depth);
            throw std::overflow_error(std::string("from ") + e.what());
        }
        throw;
    }
}

int main()
{
    for (int kind = 0; kind <= 5; kind++) run_kind(kind);
    std::printf("destroyed: %d\n", destroyed);

    try {
        nested(5);
    } catch (const std::exception &e) {
        std::printf("nested: %s: %s\n", typeid(e).name(), e.what());
    }

    std::vector<std::function<int(int)>> callbacks = {
        [](int x) { return x * 2; },
        [](int x) -> int { if (x > 2) throw std::domain_error("too big"); return x; },
    };
    for (int i = 0; i < 5; i++) {
        try {
            std::printf("callbacks: %d %d\n", callbacks[0](i), callbacks[1](i));
        } catch (const std::domain_error &e) {
            std::printf("callback %d: %s\n", i, e.what());
        }
    }

    try {
        std::vector<int> v(3);
        std::printf("%d\n", v.at(10));
    } catch (const std::out_of_range &e) {
        std::printf("vector::at: out_of_range\n");
    }

    try {
        Circle c;
        Shape &s = c;
        Square &q = dynamic_cast<Square &>(s);
        (void)q;
    } catch (const std::bad_cast &e) {
        std::printf("bad_cast caught\n");
    }

    try {
        size_t huge = static_cast<size_t>(1) << 62;
        char *p = new char[huge];
        p[0] = 0;
    } catch (const std::bad_alloc &e) {
        std::printf("bad_alloc caught\n");
    }

    try {
        Composite c(true);
    } catch (const std::runtime_error &e) {
        std::printf("Composite: %s\n", e.what());
    }

    std::exception_ptr thread_exception;
    std::thread worker([&thread_exception]() {
        try {
            Tracker t(77);
            throw std::length_error("thread exception");
        } catch (...) {
            thread_exception = std::current_exception();
        }
    });
    worker.join();
    try {
        if (thread_exception) std::rethrow_exception(thread_exception);
    } catch (const std::length_error &e) {
        std::printf("rethrown from thread: %s\n", e.what());
    }

    std::printf("destroyed: %d\n", destroyed);
    return 0;
}
