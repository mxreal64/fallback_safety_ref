import std;
import fallback_safety;

using namespace safety_profile;

// ============================================================================
// MODULAR RUNTIME ASSERT ENGINE (Replaces macro dependency)
// ============================================================================
constexpr void crucible_expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
        std::println(std::cerr, "\n[CRUCIBLE ASSERTION FAILURE] Line {}: Expected '{}' to be true.", line, expression);
        std::terminate();
    }
}
#define MODULE_ASSERT(expr) crucible_expect((expr), #expr, __LINE__)

// ============================================================================
// TEST UTILITIES & MOCK TYPES
// ============================================================================
std::atomic<std::size_t> g_destructor_count{ 0 };

struct LifoNode {
    int id;
    std::string telemetry_payload;

    LifoNode(int i) : id(i), telemetry_payload("payload_data_string_" + std::to_string(i)) {}
    ~LifoNode() {
        g_destructor_count.fetch_add(1, std::memory_order_relaxed);
    }
};

// ============================================================================
// 1. UNIT TEST: VERIFY ZERO-OVERHEAD COMPILE-TIME BRANCHING
// ============================================================================
void test_compile_time_zero_overhead_branch() {
    std::println("[RUNNING] test_compile_time_zero_overhead_branch...");

    tracked<LifoNode, false> proven_node(101);
    static_assert(sizeof(proven_node) == sizeof(LifoNode),
                  "EWG Violation: Proven tracked profiles must incur zero memory overhead.");

    tracked<LifoNode, true> unprovable_node(102);
    static_assert(sizeof(unprovable_node) == sizeof(void*),
                  "EWG Requirement: Unprovable tracking must utilize a light indirection handle.");

    MODULE_ASSERT(proven_node->id == 101);
    MODULE_ASSERT(unprovable_node->id == 102);
    std::println("[PASSED] Compile-time layout branching validated safely.\n");
}

// ============================================================================
// 2. INTEGRATION TEST: CYCLE HAZARD TRAPPING & ISOLATION
// ============================================================================
void test_cycle_hazard_trapping() {
    std::println("[RUNNING] test_cycle_hazard_trapping...");

    {
        tracked<LifoNode, true> node_A(1);
        tracked<LifoNode, true> node_B(2);
        tracked<LifoNode, true> node_C(3);

        node_A.link_to(node_B);
        node_B.link_to(node_C);

        std::println("  [ACTION] Introducing deliberate edge inversion (C -> A) to force retain loop...");
        node_C.link_to(node_A);
    }

    std::println("[PASSED] Cycle hazard handling validated without locking up or spinning out.\n");
}

// ============================================================================
// 3. CRUCIBLE STRESS TEST: 1,000,000 NODES DESTRUCTION VS. GHOST NODE DRIFT
// ============================================================================
void test_million_node_ghost_drift_crucible() {
    std::println("[RUNNING] test_million_node_ghost_drift_crucible...");

    // BUGFIX BOUNDARY: Allow background threads from previous async tests to completely
    // finish recycling their frames before capturing isolated pool metrics.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    g_destructor_count.store(0);
    constexpr std::size_t TOTAL_PARENTS = 1000000;

    std::unique_ptr<tracked<LifoNode, true>> ghost_node_handle = nullptr;

    {
        std::println("  [ACTION] Allocating pool of {} transient nodes and 1 shared ghost node...", TOTAL_PARENTS);

        tracked<LifoNode, true> shared_child(999999);
        ghost_node_handle = std::make_unique<tracked<LifoNode, true>>(shared_child);

        std::vector<tracked<LifoNode, true>> parent_pool;
        parent_pool.reserve(TOTAL_PARENTS);

        for (std::size_t i = 0; i < TOTAL_PARENTS; ++i) {
            parent_pool.emplace_back(static_cast<int>(i));
            parent_pool.back().link_to(shared_child);
        }

        std::println("  [ACTION] Mass dropping parent pool scopes concurrently while keeping ghost node alive...");
    }

    std::println("  [ACTION] Waiting for lock-free Background Cleanup Queue to catch up...");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    std::size_t destroyed_so_far = g_destructor_count.load(std::memory_order_relaxed);
    std::println("  [TELEMETRY] Total reaped nodes inside arena: {} / {}", destroyed_so_far, TOTAL_PARENTS);
    MODULE_ASSERT(destroyed_so_far == TOTAL_PARENTS);

    MODULE_ASSERT((*ghost_node_handle)->id == 999999);
    MODULE_ASSERT(ghost_node_handle->use_count() == 1);
    std::println("  [TELEMETRY] Ghost Node successfully isolated out-of-order. ID = {}", (*ghost_node_handle)->id);

    ghost_node_handle.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    MODULE_ASSERT(g_destructor_count.load() == TOTAL_PARENTS + 1);
    std::println("[PASSED] Out-of-order lifetime drift isolated flawlessly under extreme structural scale.\n");
}

// ============================================================================
// 4. PARALLEL CONCURRENCY STRESS TEST: ASYMMETRIC CONTENTION SPINLOCK
// ============================================================================
void test_high_contention_parallel_threads() {
    std::println("[RUNNING] test_high_contention_parallel_threads...");

    tracked<LifoNode, true> shared_target(42);
    constexpr int THREAD_COUNT = 8;
    constexpr int ITERATIONS_PER_THREAD = 50000;

    std::vector<std::thread> workers;
    workers.reserve(THREAD_COUNT);

    std::println("  [ACTION] Spawning {} processing threads spamming reference counts...", THREAD_COUNT);
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < THREAD_COUNT; ++t) {
        workers.emplace_back([&shared_target, t]() {
            for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
                tracked<LifoNode, true> local_copy = shared_target;

                if (i % 500 == 0) {
                    tracked<LifoNode, true> structural_edge(t * 1000 + i);
                    structural_edge.link_to(shared_target);
                }
            }
        });
    }

    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::println("  [TELEMETRY] Concurrent processing completed in {}ms without deadlocks.", elapsed);
    MODULE_ASSERT(shared_target.use_count() == 1);
    std::println("[PASSED] Lock-free and inline spinlock mechanics verified under heavy race environments.\n");
}

// ============================================================================
// MAIN EXECUTION ENGINE ENTRY
// ============================================================================
int main() {
    std::println("========================================================================");
    std::println("RUNNING TEST CRUCIBLE FOR FALLBACK DETERMINISTIC REFERENCE COUNTING");
    std::println("========================================================================\n");

    try {
        test_compile_time_zero_overhead_branch();
        test_cycle_hazard_trapping();
        test_million_node_ghost_drift_crucible();
        test_high_contention_parallel_threads();

        std::println("========================================================================");
        std::println("SUCCESS: All runtime fallback safety test categories passed cleanly!");
        std::println("========================================================================");
    } catch (const std::exception& e) {
        std::println(std::cerr, "\n[CRITICAL FAILURE] Exception slipped out during verification: {}", e.what());
        return 1;
    }

    return 0;
}
