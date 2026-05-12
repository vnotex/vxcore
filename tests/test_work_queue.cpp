#include "test_utils.h"
#include "vxcore/vxcore.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/context.h"
#include "core/work_queue.h"

// ============ WorkQueue (single queue) tests ============

int test_enqueue_and_process_next() {
  std::cout << "  Running test_enqueue_and_process_next..." << std::endl;
  vxcore::WorkQueue q;
  int value = 0;
  bool ok = q.Enqueue([&] { value = 42; });
  ASSERT_TRUE(ok);
  ASSERT_EQ(q.Size(), 1u);
  bool processed = q.ProcessNext(100);
  ASSERT_TRUE(processed);
  ASSERT_EQ(value, 42);
  ASSERT_EQ(q.Size(), 0u);
  std::cout << "  ✓ test_enqueue_and_process_next passed" << std::endl;
  return 0;
}

int test_process_all_fifo_order() {
  std::cout << "  Running test_process_all_fifo_order..." << std::endl;
  vxcore::WorkQueue q;
  std::vector<int> order;
  q.Enqueue([&] { order.push_back(1); });
  q.Enqueue([&] { order.push_back(2); });
  q.Enqueue([&] { order.push_back(3); });
  ASSERT_EQ(q.Size(), 3u);
  int count = q.ProcessAll();
  ASSERT_EQ(count, 3);
  ASSERT_EQ(q.Size(), 0u);
  ASSERT_EQ(order.size(), 3u);
  ASSERT_EQ(order[0], 1);
  ASSERT_EQ(order[1], 2);
  ASSERT_EQ(order[2], 3);
  std::cout << "  ✓ test_process_all_fifo_order passed" << std::endl;
  return 0;
}

int test_process_next_timeout_empty() {
  std::cout << "  Running test_process_next_timeout_empty..." << std::endl;
  vxcore::WorkQueue q;
  auto start = std::chrono::steady_clock::now();
  bool processed = q.ProcessNext(50);
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();
  ASSERT_FALSE(processed);
  ASSERT_TRUE(elapsed >= 40);
  std::cout << "  ✓ test_process_next_timeout_empty passed" << std::endl;
  return 0;
}

int test_size_reflects_enqueue_dequeue() {
  std::cout << "  Running test_size_reflects_enqueue_dequeue..." << std::endl;
  vxcore::WorkQueue q;
  ASSERT_EQ(q.Size(), 0u);
  q.Enqueue([] {});
  ASSERT_EQ(q.Size(), 1u);
  q.Enqueue([] {});
  ASSERT_EQ(q.Size(), 2u);
  q.ProcessNext(0);
  ASSERT_EQ(q.Size(), 1u);
  q.ProcessAll();
  ASSERT_EQ(q.Size(), 0u);
  std::cout << "  ✓ test_size_reflects_enqueue_dequeue passed" << std::endl;
  return 0;
}

int test_shutdown_unblocks_process_next() {
  std::cout << "  Running test_shutdown_unblocks_process_next..." << std::endl;
  vxcore::WorkQueue q;
  std::atomic<bool> unblocked{false};
  std::thread worker([&] {
    q.ProcessNext(-1);
    unblocked.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ASSERT_FALSE(unblocked.load());
  q.Shutdown();
  worker.join();
  ASSERT_TRUE(unblocked.load());
  std::cout << "  ✓ test_shutdown_unblocks_process_next passed" << std::endl;
  return 0;
}

int test_enqueue_after_shutdown_rejected() {
  std::cout << "  Running test_enqueue_after_shutdown_rejected..." << std::endl;
  vxcore::WorkQueue q;
  q.Shutdown();
  bool ok = q.Enqueue([] {});
  ASSERT_FALSE(ok);
  ASSERT_EQ(q.Size(), 0u);
  ASSERT_TRUE(q.IsShutdown());
  std::cout << "  ✓ test_enqueue_after_shutdown_rejected passed" << std::endl;
  return 0;
}

int test_multi_producer_single_consumer() {
  std::cout << "  Running test_multi_producer_single_consumer..." << std::endl;
  vxcore::WorkQueue q;
  std::atomic<int> counter{0};
  const int num_threads = 4;
  const int items_per_thread = 100;

  std::vector<std::thread> producers;
  for (int t = 0; t < num_threads; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < items_per_thread; ++i) {
        q.Enqueue([&] { counter.fetch_add(1); });
      }
    });
  }
  for (auto &t : producers) t.join();

  int total = num_threads * items_per_thread;
  int processed = 0;
  while (processed < total) {
    if (q.ProcessNext(100)) ++processed;
  }
  ASSERT_EQ(counter.load(), total);
  ASSERT_EQ(q.Size(), 0u);
  std::cout << "  ✓ test_multi_producer_single_consumer passed" << std::endl;
  return 0;
}

int test_producer_consumer_concurrent() {
  std::cout << "  Running test_producer_consumer_concurrent..." << std::endl;
  vxcore::WorkQueue q;
  std::atomic<int> counter{0};
  const int total_items = 200;

  std::thread consumer([&] {
    int done = 0;
    while (done < total_items) {
      if (q.ProcessNext(100)) ++done;
    }
  });

  for (int i = 0; i < total_items; ++i) {
    q.Enqueue([&] { counter.fetch_add(1); });
  }

  consumer.join();
  ASSERT_EQ(counter.load(), total_items);
  std::cout << "  ✓ test_producer_consumer_concurrent passed" << std::endl;
  return 0;
}

int test_stress_10000_items() {
  std::cout << "  Running test_stress_10000_items..." << std::endl;
  vxcore::WorkQueue q;
  std::atomic<int> counter{0};
  const int n = 10000;
  for (int i = 0; i < n; ++i) {
    q.Enqueue([&] { counter.fetch_add(1); });
  }
  ASSERT_EQ(static_cast<int>(q.Size()), n);
  int processed = q.ProcessAll();
  ASSERT_EQ(processed, n);
  ASSERT_EQ(counter.load(), n);
  ASSERT_EQ(q.Size(), 0u);
  std::cout << "  ✓ test_stress_10000_items passed" << std::endl;
  return 0;
}

int test_double_shutdown_safe() {
  std::cout << "  Running test_double_shutdown_safe..." << std::endl;
  vxcore::WorkQueue q;
  q.Shutdown();
  q.Shutdown();
  ASSERT_TRUE(q.IsShutdown());
  bool processed = q.ProcessNext(10);
  ASSERT_FALSE(processed);
  std::cout << "  ✓ test_double_shutdown_safe passed" << std::endl;
  return 0;
}

// ============ WorkQueueManager tests ============

int test_manager_get_or_create() {
  std::cout << "  Running test_manager_get_or_create..." << std::endl;
  vxcore::WorkQueueManager mgr;
  auto *q1 = mgr.GetOrCreate("sync");
  ASSERT_NOT_NULL(q1);
  auto *q2 = mgr.GetOrCreate("sync");
  ASSERT_EQ(q1, q2);
  auto *q3 = mgr.GetOrCreate("events");
  ASSERT_NOT_NULL(q3);
  ASSERT_NE(q1, q3);
  std::cout << "  ✓ test_manager_get_or_create passed" << std::endl;
  return 0;
}

int test_manager_get_nonexistent() {
  std::cout << "  Running test_manager_get_nonexistent..." << std::endl;
  vxcore::WorkQueueManager mgr;
  auto *q = mgr.Get("nope");
  ASSERT_NULL(q);
  std::cout << "  ✓ test_manager_get_nonexistent passed" << std::endl;
  return 0;
}

int test_manager_independent_queues() {
  std::cout << "  Running test_manager_independent_queues..." << std::endl;
  vxcore::WorkQueueManager mgr;
  auto *sync_q = mgr.GetOrCreate("sync");
  auto *event_q = mgr.GetOrCreate("events");

  int sync_val = 0, event_val = 0;
  sync_q->Enqueue([&] { sync_val = 1; });
  event_q->Enqueue([&] { event_val = 2; });

  ASSERT_EQ(sync_q->Size(), 1u);
  ASSERT_EQ(event_q->Size(), 1u);

  sync_q->ProcessAll();
  ASSERT_EQ(sync_val, 1);
  ASSERT_EQ(event_val, 0);

  event_q->ProcessAll();
  ASSERT_EQ(event_val, 2);
  std::cout << "  ✓ test_manager_independent_queues passed" << std::endl;
  return 0;
}

int test_manager_shutdown_all() {
  std::cout << "  Running test_manager_shutdown_all..." << std::endl;
  vxcore::WorkQueueManager mgr;
  auto *q1 = mgr.GetOrCreate("a");
  auto *q2 = mgr.GetOrCreate("b");
  mgr.ShutdownAll();
  ASSERT_TRUE(q1->IsShutdown());
  ASSERT_TRUE(q2->IsShutdown());
  std::cout << "  ✓ test_manager_shutdown_all passed" << std::endl;
  return 0;
}

int test_manager_parallel_workers() {
  std::cout << "  Running test_manager_parallel_workers..." << std::endl;
  vxcore::WorkQueueManager mgr;
  auto *sync_q = mgr.GetOrCreate("sync");
  auto *event_q = mgr.GetOrCreate("events");
  std::atomic<int> sync_count{0}, event_count{0};
  const int n = 100;

  for (int i = 0; i < n; ++i) {
    sync_q->Enqueue([&] { sync_count.fetch_add(1); });
    event_q->Enqueue([&] { event_count.fetch_add(1); });
  }

  std::thread sync_worker([&] {
    int done = 0;
    while (done < n) {
      if (sync_q->ProcessNext(100)) ++done;
    }
  });
  std::thread event_worker([&] {
    int done = 0;
    while (done < n) {
      if (event_q->ProcessNext(100)) ++done;
    }
  });

  sync_worker.join();
  event_worker.join();
  ASSERT_EQ(sync_count.load(), n);
  ASSERT_EQ(event_count.load(), n);
  std::cout << "  ✓ test_manager_parallel_workers passed" << std::endl;
  return 0;
}

// ============ C API tests ============

int test_c_api_named_queues() {
  std::cout << "  Running test_c_api_named_queues..." << std::endl;
  vxcore_set_test_mode(1);
  vxcore_clear_test_directory();

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);

  // Queue doesn't exist yet — size is 0, process returns 0
  ASSERT_EQ(vxcore_work_queue_size(ctx, "sync"), 0);
  ASSERT_EQ(vxcore_work_queue_process_next(ctx, "sync", 10), 0);

  // Create queue internally, enqueue, then process via C API
  auto *vctx = reinterpret_cast<vxcore::VxCoreContext *>(ctx);
  auto *q = vctx->work_queue_manager->GetOrCreate("sync");
  int value = 0;
  q->Enqueue([&] { value = 42; });
  ASSERT_EQ(vxcore_work_queue_size(ctx, "sync"), 1);

  int processed = vxcore_work_queue_process_next(ctx, "sync", 100);
  ASSERT_EQ(processed, 1);
  ASSERT_EQ(value, 42);
  ASSERT_EQ(vxcore_work_queue_size(ctx, "sync"), 0);

  // ProcessAll
  q->Enqueue([&] { value = 1; });
  q->Enqueue([&] { value = 2; });
  int count = vxcore_work_queue_process_all(ctx, "sync");
  ASSERT_EQ(count, 2);
  ASSERT_EQ(value, 2);

  // Shutdown single queue
  vxcore_work_queue_shutdown(ctx, "sync");
  vxcore_work_queue_shutdown(ctx, "sync");  // idempotent

  // Shutdown nonexistent queue is a no-op
  vxcore_work_queue_shutdown(ctx, "nonexistent");

  // ShutdownAll
  vctx->work_queue_manager->GetOrCreate("events");
  vxcore_work_queue_shutdown_all(ctx);

  // Null safety
  ASSERT_EQ(vxcore_work_queue_size(nullptr, "sync"), 0);
  ASSERT_EQ(vxcore_work_queue_size(ctx, nullptr), 0);

  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_c_api_named_queues passed" << std::endl;
  return 0;
}

int main() {
  // WorkQueue unit tests
  RUN_TEST(test_enqueue_and_process_next);
  RUN_TEST(test_process_all_fifo_order);
  RUN_TEST(test_process_next_timeout_empty);
  RUN_TEST(test_size_reflects_enqueue_dequeue);
  RUN_TEST(test_shutdown_unblocks_process_next);
  RUN_TEST(test_enqueue_after_shutdown_rejected);
  RUN_TEST(test_multi_producer_single_consumer);
  RUN_TEST(test_producer_consumer_concurrent);
  RUN_TEST(test_stress_10000_items);
  RUN_TEST(test_double_shutdown_safe);

  // WorkQueueManager tests
  RUN_TEST(test_manager_get_or_create);
  RUN_TEST(test_manager_get_nonexistent);
  RUN_TEST(test_manager_independent_queues);
  RUN_TEST(test_manager_shutdown_all);
  RUN_TEST(test_manager_parallel_workers);

  // C API tests
  RUN_TEST(test_c_api_named_queues);

  std::cout << "All work queue tests passed!" << std::endl;
  return 0;
}
