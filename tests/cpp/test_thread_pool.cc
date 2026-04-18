#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

#include "support/recursion_guard.h"
#include "support/thread_pool.h"
#include "test_utils.h"
using namespace xgrammar;

TEST(XGramamrThreadPoolTest, FunctionalTest) {
  ThreadPool pool(4);

  // Example 1: Use Submit to submit tasks with return values
  std::vector<std::shared_future<int>> futures;
  for (int i = 0; i < 8; ++i) {
    auto fut = pool.Submit([i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::cout << "Task " << i << " is running in thread " << std::this_thread::get_id() << "\n";
      return i * i;
    });
    futures.push_back(fut);
  }

  for (auto& fut : futures) {
    int result = fut.get();
    std::cout << "Result: " << result << "\n";
  }

  // Example 2: Use Execute to submit tasks without return values
  for (int i = 0; i < 5; ++i) {
    pool.Execute([i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::cout << "Execute task " << i << " is running in thread " << std::this_thread::get_id()
                << "\n";
    });
  }

  // Wait for task to complete
  pool.Join();
}

TEST(XGramamrThreadPoolTest, WaitRethrowsExecuteExceptionsAfterDrainingTasks) {
  ThreadPool pool(2);
  std::atomic<int> completed{0};

  pool.Execute([&] { throw std::runtime_error("boom"); });
  pool.Execute([&] { completed.fetch_add(1, std::memory_order_relaxed); });

  XGRAMMAR_EXPECT_THROW(pool.Wait(), std::exception, "boom");
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
}

TEST(XGramamrThreadPoolTest, WaitClearsStoredExceptionAfterRethrow) {
  ThreadPool pool(1);
  std::atomic<int> completed{0};

  pool.Execute([] { throw std::runtime_error("boom once"); });
  XGRAMMAR_EXPECT_THROW(pool.Wait(), std::exception, "boom once");

  pool.Execute([&] { completed.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_NO_THROW(pool.Wait());
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
}

TEST(XGramamrThreadPoolTest, JoinRethrowsExecuteExceptions) {
  ThreadPool pool(1);

  pool.Execute([] { throw std::runtime_error("join boom"); });

  XGRAMMAR_EXPECT_THROW(pool.Join(), std::exception, "join boom");
}

TEST(XGramamrThreadPoolTest, ParallelForRejectsZeroThreadCountWhenWorkExists) {
  XGRAMMAR_EXPECT_THROW(
      ParallelFor(0, 2, 0, [](int) {}), std::exception, "num_threads must be positive"
  );
  EXPECT_NO_THROW(ParallelFor(0, 0, 0, [](int) {}));
}

TEST(XGrammarRecursionGuardTest, ConstructorRollbackOnThrow) {
  int recursion_depth = 0;
  int previous_max_depth = RecursionGuard::GetMaxRecursionDepth();
  RecursionGuard::SetMaxRecursionDepth(1);

  {
    RecursionGuard outer(&recursion_depth);
    EXPECT_EQ(recursion_depth, 1);
    XGRAMMAR_EXPECT_THROW(RecursionGuard inner(&recursion_depth), std::exception, "Maximum recursion depth exceeded");
    EXPECT_EQ(recursion_depth, 1);
  }

  EXPECT_EQ(recursion_depth, 0);
  RecursionGuard::SetMaxRecursionDepth(previous_max_depth);
}

// TEST(XGramamrThreadPoolTest, PressureTest) {
//   const size_t num_threads = std::thread::hardware_concurrency();
//   ThreadPool pool(num_threads);

//   const size_t num_tasks = 1000;
//   int counter = 0;
//   std::mutex counter_mutex;

//   auto start_time = std::chrono::high_resolution_clock::now();

//   for (size_t i = 0; i < num_tasks; ++i) {
//     pool.Execute([&counter, &counter_mutex, i]() {
//       std::this_thread::sleep_for(std::chrono::milliseconds(i % 50));
//       std::lock_guard<std::mutex> lock(counter_mutex);
//       counter++;
//     });
//   }

//   pool.Wait();

//   auto end_time = std::chrono::high_resolution_clock::now();

//   EXPECT_EQ(counter, static_cast<int>(num_tasks));

//   auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
//   std::cout << "Pressure test completed, time taken: " << duration.count() << " milliseconds.\n";
// }
