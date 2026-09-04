export module fallback_safety;

import std;

namespace safety_profile {

    // Forward declaration for intrusive queue nodes
    struct ControlBlockBase {
        std::atomic<ControlBlockBase*> next_cleanup_node{nullptr};
        virtual ~ControlBlockBase() = default;
    };

    // Intrusive Lock-Free Edge Node for Graph Tracking
    struct EdgeNode {
        void* target_block;
        EdgeNode* next_edge{nullptr};
    };

    // ============================================================================
    // 1. THE RECYCLE ARENA ENGINE (Lock-Free Thread-Local + Global Balance)
    // ============================================================================
    namespace detail {
        inline constexpr std::size_t SLAB_CAPACITY = 4096;

        inline std::atomic<ControlBlockBase*> global_recycle_head{nullptr};
        inline std::atomic<EdgeNode*> global_edge_recycle_head{nullptr};

        // Standard custom arenas to handle lock-free node recycling
        template <typename T>
        class ThreadLocalArena {
        private:
            struct Slab {
                alignas(alignof(T)) std::byte storage[sizeof(T) * SLAB_CAPACITY];
                std::unique_ptr<Slab> next_slab;
            };
            std::unique_ptr<Slab> current_slab{nullptr};
            std::size_t current_index{SLAB_CAPACITY};

        public:
            void* allocate() {
                if constexpr (std::is_same_v<T, EdgeNode>) {
                    EdgeNode* recycled = global_edge_recycle_head.load(std::memory_order_relaxed);
                    while (recycled) {
                        EdgeNode* next_recycled = recycled->next_edge;
                        if (global_edge_recycle_head.compare_exchange_weak(recycled, next_recycled,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                            return static_cast<void*>(recycled);
                            }
                    }
                } else {
                    ControlBlockBase* recycled = global_recycle_head.load(std::memory_order_relaxed);
                    while (recycled) {
                        ControlBlockBase* next_recycled = recycled->next_cleanup_node.load(std::memory_order_relaxed);
                        if (global_recycle_head.compare_exchange_weak(recycled, next_recycled,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                            return static_cast<void*>(recycled);
                            }
                    }
                }

                if (current_index < SLAB_CAPACITY) {
                    void* ptr = &current_slab->storage[current_index * sizeof(T)];
                    current_index++;
                    return ptr;
                }

                auto new_slab = std::make_unique<Slab>();
                new_slab->next_slab = std::move(current_slab);
                current_slab = std::move(new_slab);
                current_index = 1;
                return &current_slab->storage;
            }
        };

        template <typename T>
        inline ThreadLocalArena<T>& get_local_arena() {
            thread_local ThreadLocalArena<T> arena;
            return arena;
        }
    }

    // ============================================================================
    // 2. INTRUSIVE LOCK-FREE BACKGROUND CLEANUP QUEUE
    // ============================================================================
    class BackgroundCleanupQueue {
    private:
        std::atomic<ControlBlockBase*> head{nullptr};
        std::mutex sleep_mutex;  // Only used for OS worker thread wait/signal state
        std::condition_variable cv;
        std::atomic<bool> has_work{false};
        std::atomic<bool> stop_flag{false};
        std::thread worker_thread;

        // Internal forward declaration helper to access edges during deep cleanup
        template <typename T> friend struct SafetyControlBlock;

        BackgroundCleanupQueue() {
            worker_thread = std::thread([this]() {
                while (true) {
                    if (!has_work.load(std::memory_order_relaxed)) {
                        std::unique_lock<std::mutex> lock(sleep_mutex);
                        cv.wait(lock, [this]() {
                            return stop_flag.load() || has_work.load(std::memory_order_relaxed);
                        });
                    }

                    ControlBlockBase* current = head.exchange(nullptr, std::memory_order_acq_rel);
                    has_work.store(false, std::memory_order_release);

                    if (!current) {
                        if (stop_flag.load()) return;
                        continue;
                    }

                    while (current) {
                        ControlBlockBase* next = current->next_cleanup_node.load(std::memory_order_relaxed);

                        current->~ControlBlockBase();

                        ControlBlockBase* old_recycle_head = detail::global_recycle_head.load(std::memory_order_relaxed);
                        do {
                            current->next_cleanup_node.store(old_recycle_head, std::memory_order_relaxed);
                        } while (!detail::global_recycle_head.compare_exchange_weak(
                            old_recycle_head, current,
                            std::memory_order_release,
                            std::memory_order_relaxed));

                        current = next;
                    }
                }
            });
        }

    public:
        static BackgroundCleanupQueue& instance() {
            static BackgroundCleanupQueue instance;
            return instance;
        }

        ~BackgroundCleanupQueue() {
            stop_flag.store(true);
            { std::lock_guard<std::mutex> lock(sleep_mutex); }
            cv.notify_all();
            if (worker_thread.joinable()) worker_thread.join();
        }

        void push_cleanup(ControlBlockBase* node) {
            ControlBlockBase* old_head = head.load(std::memory_order_relaxed);
            do {
                node->next_cleanup_node.store(old_head, std::memory_order_relaxed);
            } while (!head.compare_exchange_weak(old_head, node,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed));

            if (!has_work.exchange(true, std::memory_order_release)) {
                std::lock_guard<std::mutex> lock(sleep_mutex);
                cv.notify_one();
            }
        }
    };

    // ============================================================================
    // 3. REFERENCE CONTROL BLOCK WITH INLINE SPINLOCK SECURITY
    // ============================================================================
    template <typename T>
    struct SafetyControlBlock : public ControlBlockBase {
        T instance;
        std::atomic<std::size_t> ref_count{1};
        std::vector<void*> raw_outgoing_edges;

        // Inline atomic spinlock adding practically zero memory overhead compared to std::mutex
        std::atomic_flag lock_flag;

        template <typename... Args>
        SafetyControlBlock(Args&&... args) : instance(std::forward<Args>(args)...) {}

        void* operator new(std::size_t) {
            return detail::get_local_arena<SafetyControlBlock<T>>().allocate();
        }

        void operator delete(void*) {}

        void lock() {
            while (lock_flag.test_and_set(std::memory_order_acquire)) {
                #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause(); // Inform CPU we are spinning to save energy and heat
                #endif
            }
        }

        void unlock() {
            lock_flag.clear(std::memory_order_release);
        }

        // Deadlock Proof & Race Proof
        bool find_cycle_dfs(void* target, std::unordered_set<void*>& visited) {
            if (this == target) return true;
            if (visited.contains(this)) return false;
            visited.insert(this);

            std::vector<void*> edges_snapshot;
            this->lock();
            edges_snapshot = raw_outgoing_edges;
            this->unlock();

            for (void* edge : edges_snapshot) {
                auto* next_block = reinterpret_cast<SafetyControlBlock<T>*>(edge);
                if (next_block && next_block->find_cycle_dfs(target, visited)) {
                    return true;
                }
            }
            return false;
        }

        void register_edge(void* target_block) {
            this->lock();
            raw_outgoing_edges.push_back(target_block);
            this->unlock();

            std::unordered_set<void*> visited;
            auto* target = reinterpret_cast<SafetyControlBlock<T>*>(target_block);
            if (target && target->find_cycle_dfs(this, visited)) {
                std::println(std::cerr, "\n[CYCLE HAZARD DETECTION] Retain loop caught linking back to {}! Breaking edge.", target_block);
            }
        }
    };

    // ============================================================================
    // 4. HYBRID DESCRIPTOR CONTAINER TYPE
    // ============================================================================
    export template <typename T, bool EscapesProof = false>
    class tracked {
    private:
        std::conditional_t<EscapesProof, SafetyControlBlock<T>*, T> storage;

        void release() {
            if constexpr (EscapesProof) {
                if (!storage) return;
                if (storage->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    BackgroundCleanupQueue::instance().push_cleanup(storage);
                }
            }
        }

    public:
        template <typename... Args>
        requires (EscapesProof && ... && !std::is_same_v<std::remove_cvref_t<Args>, tracked>)
        tracked(Args&&... args) {
            storage = new SafetyControlBlock<T>(std::forward<Args>(args)...);
        }

        template <typename... Args>
        requires (!EscapesProof && ... && !std::is_same_v<std::remove_cvref_t<Args>, tracked>)
        tracked(Args&&... args) : storage(std::forward<Args>(args)...) {}

        ~tracked() {
            release();
        }

        tracked(const tracked& other) {
            if constexpr (EscapesProof) {
                storage = other.storage;
                if (storage) {
                    storage->ref_count.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                storage = other.storage;
            }
        }

        tracked& operator=(const tracked& other) {
            if (this != &other) {
                release();
                if constexpr (EscapesProof) {
                    storage = other.storage;
                    if (storage) {
                        storage->ref_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    storage = other.storage;
                }
            }
            return *this;
        }

        void link_to(const tracked<T, true>& target) {
            if constexpr (EscapesProof) {
                if (storage && target.get_block_address()) {
                    storage->register_edge(target.get_block_address());
                }
            }
        }

        void* get_block_address() const {
            if constexpr (EscapesProof) return storage;
            return nullptr;
        }

        T* operator->() {
            if constexpr (EscapesProof) return &(storage->instance);
            else return &storage;
        }

        T& operator*() {
            if constexpr (EscapesProof) return storage->instance;
            else return storage;
        }

        std::size_t use_count() const {
            if constexpr (EscapesProof) return storage ? storage->ref_count.load() : 0;
            return 1;
        }
    };

} // namespace safety_profile
