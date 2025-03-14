/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#include <exception>
#include <numeric>

#include <seastar/core/future-util.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/later.hh>
#include <seastar/core/thread.hh>
#include <seastar/testing/random.hh>

using namespace seastar;
using namespace std::chrono_literals;

#ifndef SEASTAR_COROUTINES_ENABLED

SEASTAR_TEST_CASE(test_coroutines_not_compiled_in) {
    return make_ready_future<>();
}

#else

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/switch_to.hh>
#include <seastar/coroutine/parallel_for_each.hh>

namespace {

future<int> old_fashioned_continuations() {
    return yield().then([] {
        return 42;
    });
}

future<int> simple_coroutine() {
    co_await yield();
    co_return 53;
}

future<int> ready_coroutine() {
    co_return 64;
}

future<std::tuple<int, double>> tuple_coroutine() {
    co_return std::tuple(1, 2.);
}

future<int> failing_coroutine() {
    co_await yield();
    throw 42;
}

[[gnu::noinline]] int throw_exception(int x) {
    throw x;
}

future<int> failing_coroutine2() noexcept {
    co_await yield();
    co_return throw_exception(17);
}

}

SEASTAR_TEST_CASE(test_simple_coroutines) {
    BOOST_REQUIRE_EQUAL(co_await old_fashioned_continuations(), 42);
    BOOST_REQUIRE_EQUAL(co_await simple_coroutine(), 53);
    BOOST_REQUIRE_EQUAL(ready_coroutine().get0(), 64);
    BOOST_REQUIRE(co_await tuple_coroutine() == std::tuple(1, 2.));
    BOOST_REQUIRE_EXCEPTION((void)co_await failing_coroutine(), int, [] (auto v) { return v == 42; });
    BOOST_CHECK_EQUAL(co_await failing_coroutine().then_wrapped([] (future<int> f) -> future<int> {
        BOOST_REQUIRE(f.failed());
        try {
            std::rethrow_exception(f.get_exception());
        } catch (int v) {
           co_return v;
        }
    }), 42);
    BOOST_REQUIRE_EXCEPTION((void)co_await failing_coroutine2(), int, [] (auto v) { return v == 17; });
    BOOST_CHECK_EQUAL(co_await failing_coroutine2().then_wrapped([] (future<int> f) -> future<int> {
        BOOST_REQUIRE(f.failed());
        try {
            std::rethrow_exception(f.get_exception());
        } catch (int v) {
           co_return v;
        }
    }), 17);
}

SEASTAR_TEST_CASE(test_abandond_coroutine) {
    std::optional<future<int>> f;
    {
        auto p1 = promise<>();
        auto p2 = promise<>();
        auto p3 = promise<>();
        f = p1.get_future().then([&] () -> future<int> {
            p2.set_value();
            BOOST_CHECK_THROW(co_await p3.get_future(), broken_promise);
            co_return 1;
        });
        p1.set_value();
        co_await p2.get_future();
    }
    BOOST_CHECK_EQUAL(co_await std::move(*f), 1);
}

SEASTAR_TEST_CASE(test_scheduling_group) {
    auto other_sg = co_await create_scheduling_group("the other group", 10.f);
    std::exception_ptr ex;

    try {
        auto p1 = promise<>();
        auto p2 = promise<>();

        auto p1b = promise<>();
        auto p2b = promise<>();
        auto f1 = p1b.get_future();
        auto f2 = p2b.get_future();

        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        auto f_ret = with_scheduling_group(other_sg,
                [other_sg_cap = other_sg] (future<> f1, future<> f2, promise<> p1, promise<> p2) -> future<int> {
            // Make a copy in the coroutine before the lambda is destroyed.
            auto other_sg = other_sg_cap;
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            BOOST_REQUIRE(other_sg == other_sg);
            p1.set_value();
            co_await std::move(f1);
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            p2.set_value();
            co_await std::move(f2);
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            co_return 42;
        }, p1.get_future(), p2.get_future(), std::move(p1b), std::move(p2b));

        co_await std::move(f1);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        p1.set_value();
        co_await std::move(f2);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        p2.set_value();
        BOOST_REQUIRE_EQUAL(co_await std::move(f_ret), 42);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
    } catch (...) {
        ex = std::current_exception();
    }
    co_await destroy_scheduling_group(other_sg);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

SEASTAR_TEST_CASE(test_switch_to) {
    auto other_sg0 = co_await create_scheduling_group("other group 0", 10.f);
    auto other_sg1 = co_await create_scheduling_group("other group 1", 10.f);
    std::exception_ptr ex;

    try {
        auto base_sg = current_scheduling_group();

        auto prev_sg = co_await coroutine::switch_to(other_sg0);
        BOOST_REQUIRE(current_scheduling_group() == other_sg0);
        BOOST_REQUIRE(prev_sg == base_sg);

        auto same_sg = co_await coroutine::switch_to(other_sg0);
        BOOST_REQUIRE(current_scheduling_group() == other_sg0);
        BOOST_REQUIRE(same_sg == other_sg0);

        auto nested_sg = co_await coroutine::switch_to(other_sg1);
        BOOST_REQUIRE(current_scheduling_group() == other_sg1);
        BOOST_REQUIRE(nested_sg == other_sg0);

        co_await coroutine::switch_to(base_sg);
        BOOST_REQUIRE(current_scheduling_group() == base_sg);
    } catch (...) {
        ex = std::current_exception();
    }

    co_await destroy_scheduling_group(other_sg1);
    co_await destroy_scheduling_group(other_sg0);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

future<> check_thread_inherits_sg_from_coroutine_frame(scheduling_group expected_sg) {
    return seastar::async([expected_sg] {
        BOOST_REQUIRE(current_scheduling_group() == expected_sg);
    });
}

future<> check_coroutine_inherits_sg_from_another_one(scheduling_group expected_sg) {
    co_await yield();
    BOOST_REQUIRE(current_scheduling_group() == expected_sg);
}

future<> switch_to_sg_and_perform_inheriting_checks(scheduling_group base_sg, scheduling_group new_sg) {
    BOOST_REQUIRE(current_scheduling_group() == base_sg);
    co_await coroutine::switch_to(new_sg);
    BOOST_REQUIRE(current_scheduling_group() == new_sg);

    co_await check_thread_inherits_sg_from_coroutine_frame(new_sg);
    co_await check_coroutine_inherits_sg_from_another_one(new_sg);

    // don't restore previous sg on purpose, expecting it will be restored once coroutine goes out of scope
}

SEASTAR_TEST_CASE(test_switch_to_sg_restoration_and_inheriting) {
    auto new_sg = co_await create_scheduling_group("other group 0", 10.f);
    std::exception_ptr ex;

    try {
        auto base_sg = current_scheduling_group();

        co_await switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg);
        // seastar automatically restores base_sg once it goes out of coroutine frame
        BOOST_REQUIRE(current_scheduling_group() == base_sg);

        co_await seastar::async([base_sg, new_sg] {
            switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg).get();
            BOOST_REQUIRE(current_scheduling_group() == base_sg);
        });

        co_await switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg).finally([base_sg] {
            BOOST_REQUIRE(current_scheduling_group() == base_sg);
        });
    } catch (...) {
        ex = std::current_exception();
    }

    co_await destroy_scheduling_group(new_sg);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

SEASTAR_TEST_CASE(test_preemption) {
    bool x = false;
    unsigned preempted = 0;
    auto f = yield().then([&x] {
            x = true;
        });

    // try to preempt 1000 times. 1 should be enough if not for
    // task queue shaffling in debug mode which may cause co-routine
    // continuation to run first.
    while(preempted < 1000 && !x) {
        preempted += need_preempt(); 
        co_await make_ready_future<>();
    }
    auto save_x = x;
    // wait for yield() to complete
    co_await std::move(f);
    BOOST_REQUIRE(save_x);
    co_return;
}

SEASTAR_TEST_CASE(test_no_preemption) {
    bool x = false;
    unsigned preempted = 0;
    auto f = yield().then([&x] {
            x = true;
        });

    // preemption should not happen, we explicitly asked for continuing if possible
    while(preempted < 1000 && !x) {
        preempted += need_preempt();
        co_await coroutine::without_preemption_check(make_ready_future<>());
    }
    auto save_x = x;
    // wait for yield() to complete
    co_await std::move(f);
    BOOST_REQUIRE(!save_x);
    co_return;
}

SEASTAR_TEST_CASE(test_all_simple) {
    auto [a, b] = co_await coroutine::all(
        [] { return make_ready_future<int>(1); },
        [] { return make_ready_future<int>(2); }
    );
    BOOST_REQUIRE_EQUAL(a, 1);
    BOOST_REQUIRE_EQUAL(b, 2);
}

SEASTAR_TEST_CASE(test_all_permutations) {
    std::vector<std::chrono::milliseconds> delays = { 0ms, 0ms, 2ms, 2ms, 4ms, 6ms };
    auto make_delayed_future_returning_nr = [&] (int nr) {
        return [=] {
            auto delay = delays[nr];
            return delay == 0ms ? make_ready_future<int>(nr) : sleep(delay).then([nr] { return make_ready_future<int>(nr); });
        };
    };
    do {
        auto [a, b, c, d, e, f] = co_await coroutine::all(
            make_delayed_future_returning_nr(0),
            make_delayed_future_returning_nr(1),
            make_delayed_future_returning_nr(2),
            make_delayed_future_returning_nr(3),
            make_delayed_future_returning_nr(4),
            make_delayed_future_returning_nr(5)
        );
        BOOST_REQUIRE_EQUAL(a, 0);
        BOOST_REQUIRE_EQUAL(b, 1);
        BOOST_REQUIRE_EQUAL(c, 2);
        BOOST_REQUIRE_EQUAL(d, 3);
        BOOST_REQUIRE_EQUAL(e, 4);
        BOOST_REQUIRE_EQUAL(f, 5);
    } while (std::ranges::next_permutation(delays).found);
}

SEASTAR_TEST_CASE(test_all_ready_exceptions) {
    try {
        co_await coroutine::all(
            [] () -> future<> { throw 1; },
            [] () -> future<> { throw 2; }
        );
    } catch (int e) {
        BOOST_REQUIRE(e == 1 || e == 2);
    }
}

SEASTAR_TEST_CASE(test_all_nonready_exceptions) {
    try {
        co_await coroutine::all(
            [] () -> future<> { 
                co_await sleep(1ms);
                throw 1;
            },
            [] () -> future<> { 
                co_await sleep(1ms);
                throw 2;
            }
        );
    } catch (int e) {
        BOOST_REQUIRE(e == 1 || e == 2);
    }
}

SEASTAR_TEST_CASE(test_all_heterogeneous_types) {
    auto [a, b] = co_await coroutine::all(
        [] () -> future<int> { 
            co_await sleep(1ms);
            co_return 1;
        },
        [] () -> future<> { 
            co_await sleep(1ms);
        },
        [] () -> future<long> { 
            co_await sleep(1ms);
            co_return 2L;
        }
    );
    BOOST_REQUIRE_EQUAL(a, 1);
    BOOST_REQUIRE_EQUAL(b, 2L);
}

SEASTAR_TEST_CASE(test_all_noncopyable_types) {
    auto [a] = co_await coroutine::all(
        [] () -> future<std::unique_ptr<int>> {
            co_return std::make_unique<int>(6);
        }
    );
    BOOST_REQUIRE_EQUAL(*a, 6);
}

SEASTAR_TEST_CASE(test_all_throw_in_input_func) {
    int nr_completed = 0;
    bool exception_seen = false;
    try {
        co_await coroutine::all(
            [&] () -> future<int> {
                co_await sleep(1ms);
                ++nr_completed;
                co_return 7;
            },
            [&] () -> future<int> {
                throw 9;
            },
            [&] () -> future<int> {
                co_await sleep(1ms);
                ++nr_completed;
                co_return 7;
            }
        );
    } catch (int n) {
        BOOST_REQUIRE_EQUAL(n, 9);
        exception_seen = true;
    }
    BOOST_REQUIRE_EQUAL(nr_completed, 2);
    BOOST_REQUIRE(exception_seen);
}

SEASTAR_TEST_CASE(test_coroutine_exception) {
    auto i_am_exceptional = [] () -> future<int> {
        co_return coroutine::exception(std::make_exception_ptr(std::runtime_error("threw")));
    };
    BOOST_REQUIRE_THROW(co_await i_am_exceptional(), std::runtime_error);
    co_await i_am_exceptional().then_wrapped([] (future<int> f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::runtime_error);
    });

    auto i_am_exceptional_as_well = [] () -> future<bool> {
        co_return coroutine::make_exception(std::logic_error("threw"));
    };
    BOOST_REQUIRE_THROW(co_await i_am_exceptional_as_well(), std::logic_error);
    co_await i_am_exceptional_as_well().then_wrapped([] (future<bool> f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::logic_error);
    });
}

SEASTAR_TEST_CASE(test_maybe_yield) {
    int var = 0;
    bool done = false;
    auto spinner = [&] () -> future<> {
        // increment a variable continuously, but yield so an observer can see it.
        while (!done) {
            ++var;
            co_await coroutine::maybe_yield();
        }
    };
    auto spinner_fut = spinner();
    int snapshot = var;
    for (int nr_changes = 0; nr_changes < 10; ++nr_changes) {
        // Try to observe the value changing in time, yield to
        // allow the spinner to advance it.
        while (snapshot == var) {
            co_await coroutine::maybe_yield();
        }
        snapshot = var;
    }
    done = true;
    co_await std::move(spinner_fut);
    BOOST_REQUIRE(true); // the test will hang if it doesn't work.
}

#if __has_include(<coroutine>) && !defined(__clang__)

#include "tl-generator.hh"
tl::generator<int> simple_generator(int max)
{
    for (int i = 0; i < max; ++i) {
        co_yield i;
    }
}

SEASTAR_TEST_CASE(generator)
{
    // test ability of seastar::parallel_for_each to deal with move-only views
    int accum = 0;
    co_await seastar::parallel_for_each(simple_generator(10), [&](int i) {
        accum += i;
        return seastar::make_ready_future<>();
    });
    BOOST_REQUIRE_EQUAL(accum, 45);

    // test ability of seastar::max_concurrent_for_each to deal with move-only views
    accum = 0;
    co_await seastar::max_concurrent_for_each(simple_generator(10), 10, [&](int i) {
        accum += i;
        return seastar::make_ready_future<>();
    });
    BOOST_REQUIRE_EQUAL(accum, 45);
}

#endif

SEASTAR_TEST_CASE(test_parallel_for_each_empty) {
    std::vector<int> values;
    int count = 0;

    co_await coroutine::parallel_for_each(values, [&] (int x) {
        ++count;
    });
    BOOST_REQUIRE_EQUAL(count, 0); // the test will hang if it doesn't work.
}

SEASTAR_TEST_CASE(test_parallel_for_each_exception) {
    std::array<int, 5> values = { 10, 2, 1, 4, 8 };
    int count = 0;
    auto& eng = testing::local_random_engine;
    auto dist = std::uniform_int_distribution<unsigned>();
    int throw_at = dist(eng) % values.size();

    BOOST_TEST_MESSAGE(fmt::format("Will throw at value #{}/{}", throw_at, values.size()));

    auto f0 = coroutine::parallel_for_each(values, [&] (int x) {
        if (count++ == throw_at) {
            BOOST_TEST_MESSAGE("throw");
            throw std::runtime_error("test");
        }
    });
    // An exception thrown by the functor must be propagated
    BOOST_REQUIRE_THROW(co_await std::move(f0), std::runtime_error);
    // Functor must be called on all values, even if there's an exception
    BOOST_REQUIRE_EQUAL(count, values.size());

    count = 0;
    throw_at = dist(eng) % values.size();
    BOOST_TEST_MESSAGE(fmt::format("Will throw at value #{}/{}", throw_at, values.size()));

    auto f1 = coroutine::parallel_for_each(values, [&] (int x) -> future<> {
        co_await sleep(std::chrono::milliseconds(x));
        if (count++ == throw_at) {
            throw std::runtime_error("test");
        }
    });
    BOOST_REQUIRE_THROW(co_await std::move(f1), std::runtime_error);
    BOOST_REQUIRE_EQUAL(count, values.size());
}

SEASTAR_TEST_CASE(test_parallel_for_each) {
    std::vector<int> values = { 3, 1, 4 };
    int sum_of_squares = 0;

    int expected = std::accumulate(values.begin(), values.end(), 0, [] (int sum, int x) {
        return sum + x * x;
    });

    // Test all-ready futures
    co_await coroutine::parallel_for_each(values, [&sum_of_squares] (int x) {
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, expected);

    // Test non-ready futures
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(values, [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, expected);

    // Test legacy subrange
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(values.begin(), values.end() - 1, [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, 10);

    // clang 13.0.1 doesn't support subrange
    // so provide also a Iterator/Sentinel based constructor.
    // See https://github.com/llvm/llvm-project/issues/46091
#ifndef __clang__
    // Test std::ranges::subrange
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(std::ranges::subrange(values.begin(), values.end() - 1), [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, 10);
#endif
}

#endif
