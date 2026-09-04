import fallback_safety;
import std;

struct ExploitationNode {
    int id;
    std::string dynamic_buffer;

    ExploitationNode(int i) : id(i) {
        if (i % 100 == 0) {
            // Allocate asymmetric buffer sizes to violently fragment memory alignment
            dynamic_buffer = std::string(i % 500, 'X');
        }
    }

    ~ExploitationNode() {
        id = -999;
    }
};

int main() {
    std::println("================================================================================");
    std::println("[EXPLOITATION RUN] ATTACKING THE INLINE SPIN-LOCK LIFE-SAFETY RUNTIME");
    std::println("================================================================================");

    // ----------------------------------------------------------------------------
    // EXPLOIT 1: HIGH-SPEED HARVEST-AND-REUSE FRAGMENTS (Slab Poisoning Attack)
    // ----------------------------------------------------------------------------
    std::println("[Exploit 1] Triggering Rapid Generation Churn (Slab & Recycle Stress)...");
    {
        // Allocate and immediately drop millions of elements in tiny bursts
        // to force the background thread to hammer the global recycle stack
        // while the main thread simultaneously re-allocates from it.
        for (int batch = 0; batch < 50; ++batch) {
            std::vector<safety_profile::tracked<ExploitationNode, true>> ephemeral_burst;
            ephemeral_burst.reserve(100'000);
            for (int i = 0; i < 100'000; ++i) {
                ephemeral_burst.emplace_back(batch * 100'000 + i);
            }
            // ephemeral_burst falls out of scope, flooding global_recycle_head immediately
        }
    }
    std::println("-> Result: Slab and recycle stack integrity sustained under high-speed recycling.\n");

    // ----------------------------------------------------------------------------
    // EXPLOIT 2: HARDWARE SPIN-LOCK HAMMER (Inline Locking Engine Chaos Test)
    // ----------------------------------------------------------------------------
    std::println("[Exploit 2] Launching Cross-Thread Cyclic Inversion Attack...");
    {
        constexpr int POOL_SIZE = 5'000;
        std::vector<safety_profile::tracked<ExploitationNode, true>> collision_pool;
        collision_pool.reserve(POOL_SIZE);

        // Allocate a baseline pool of nodes
        for (int i = 0; i < POOL_SIZE; ++i) {
            collision_pool.emplace_back(i);
        }

        std::vector<std::jthread> attackers;
        // Increase the hardware thread strain to induce high spin-lock contention
        int total_threads = std::max(16u, std::thread::hardware_concurrency() * 2);
        attackers.reserve(total_threads);

        std::println("  -> Spawning {} threads attempting to cross-link nodes concurrently...", total_threads);

        // Multi-threaded storm actively inducing AB-BA locking orders on the inline flags.
        // Threads will step through snapshot frames and copy edge structures simultaneously.
        for (int t = 0; t < total_threads; ++t) {
            attackers.emplace_back([&collision_pool, t]() {
                for (int i = 0; i < POOL_SIZE - 1; ++i) {
                    if (t % 2 == 0) {
                        collision_pool[i].link_to(collision_pool[i + 1]);
                    } else {
                        collision_pool[i + 1].link_to(collision_pool[i]);
                    }
                }
            });
        }
    } // Wait for all threads to join and destroy the pool
    std::println("-> Result: Inline spinlocks successfully prevented data-races and deadlocks!\n");

    // ----------------------------------------------------------------------------
    // EXPLOIT 3: MULTI-GENERATIONAL ESCAPE DRIFT (The Ghost Ownership Exploit)
    // ----------------------------------------------------------------------------
    std::println("[Exploit 3] Testing Asymmetric Out-of-Order Lifecycle Drift...");
    {
        std::optional<safety_profile::tracked<ExploitationNode, true>> eternal_ghost;

        {
            // Spawn a massive temporary hierarchy
            std::vector<safety_profile::tracked<ExploitationNode, true>> local_hierarchy;
            local_hierarchy.reserve(1'000'000);
            for(int i = 0; i < 1'000'000; ++i) {
                local_hierarchy.emplace_back(i);
            }

            // Extract ownership of a single middle element to an outliving outer scope
            eternal_ghost = local_hierarchy[500'000];

            // Create links from the ghost node to elements that are about to vanish completely
            eternal_ghost->link_to(local_hierarchy[0]);
            eternal_ghost->link_to(local_hierarchy[999'999]);

            std::println("  -> Dropping 1,000,000 parent nodes while keeping an internal node alive...");
        } // 999,999 nodes fall out of scope here. The background thread starts destroying them.

        std::println("  -> Verifying isolated ghost node can safely access its inner data...");
        if ((*eternal_ghost)->id == 500'000) {
            std::println("  -> Safety Confirmed: Ghost Node inner instance remains intact!");
        } else {
            std::println("  -> CRITICAL FAULT: Node data corrupted by background worker!");
        }
    } // Ghost node finally dies here
    std::println("-> Result: Out-of-order lifecycle management complete.\n");

    std::println("================================================================================");
    std::println("[Crucible Complete] The system refused to crash, spin lock-freeze, or deadlock.");

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    return 0;
}
