#include "profiling/memory_profiler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <assert.h>
#include <malloc.h>
#include <vector>
#include <algorithm>
#include <execinfo.h>
#include <cstring>

namespace {
  template<typename T>
  class mmap_allocator : public std::allocator<T> {
   public:
    typedef size_t size_type;
    typedef T *pointer;
    typedef const T *const_pointer;

    template <typename _Tp1>
    struct rebind {
      typedef mmap_allocator<_Tp1> other;
    };

    pointer allocate(size_type n, const void *hint = 0) {
      return (pointer)malloc(n * sizeof(T));
    }

    void deallocate(pointer p, size_type n) {
      return free(p);
    }

    mmap_allocator() throw() : std::allocator<T>() {}
    mmap_allocator(const mmap_allocator &a) throw() : std::allocator<T>(a) {}
    template <class U>
    mmap_allocator(const mmap_allocator<U> &a) throw() : std::allocator<T>(a) {}
    ~mmap_allocator() throw() {}
  };

  static constexpr size_t kBufferSize = 32 * 1024ull;

  struct AllocationDescription {
    uint64_t count;
    uint64_t alloc_size;
    char stack[kBufferSize];

    AllocationDescription(AllocationDescription const &) = delete;
    AllocationDescription(AllocationDescription &&) = delete;
    AllocationDescription &operator=(AllocationDescription const &) = delete;
    AllocationDescription &operator=(AllocationDescription &&) = delete;
    AllocationDescription() : count(0ull), alloc_size(0ull) {
      stack[0] = 0;
    }
  };

  using HashTableType = size_t;
  using AllocationsTableType = std::unordered_map<
      HashTableType,
      AllocationDescription,
      std::hash<HashTableType>,
      std::equal_to<HashTableType>,
      mmap_allocator<std::pair<const HashTableType, AllocationDescription>>>;
  using PointersTableType = std::unordered_map<
      uintptr_t,
      HashTableType,
      std::hash<uintptr_t>,
      std::equal_to<uintptr_t>,
      mmap_allocator<std::pair<const uintptr_t, HashTableType>>>;

  char allocations_table[sizeof(AllocationsTableType)];
  AllocationsTableType &allocationsTable() {
    return *(AllocationsTableType *)allocations_table;
  }

  char pointers_table[sizeof(PointersTableType)];
  PointersTableType &pointersTable() {
    return *(PointersTableType *)pointers_table;
  }

  std::mutex tables_cs;
  std::atomic<bool> table_ready = false;

  char track[kBufferSize + 1];
  void makeDelete(void *ptr) {
    if (table_ready.load()) {
      std::lock_guard lock(tables_cs);
      if (auto it = pointersTable().find(uintptr_t(ptr));
          it != pointersTable().end()) {
        auto const hash = it->second;
        pointersTable().erase(it);

        auto it_hash = allocationsTable().find(hash);
        assert(it_hash != allocationsTable().end());

        it_hash->second.alloc_size -= malloc_usable_size(ptr);
        if (--it_hash->second.count == 0ull)
          allocationsTable().erase(it_hash);
      }
    }
    free(ptr);
  }
}

namespace profiler {
  void initTables() {
    std::lock_guard lock(tables_cs);
    new (allocations_table) AllocationsTableType();
    new (pointers_table) PointersTableType();
    table_ready.store(true);
  }

  void deinitTables() {
    std::lock_guard lock(tables_cs);
    table_ready.store(false);
    allocationsTable().~AllocationsTableType();
    pointersTable().~PointersTableType();
  }

  void printTables() {
    if (!table_ready.load())
      return;

    std::lock_guard lock(tables_cs);

    std::vector<AllocationDescription *,
                mmap_allocator<AllocationDescription *>>
        descriptors;
    descriptors.reserve(allocationsTable().size());
    for (auto &it : allocationsTable()) descriptors.push_back(&it.second);

    std::sort(descriptors.begin(), descriptors.end(), [](auto a, auto b) {
      return a->alloc_size > b->alloc_size;
    });

    for (auto &item : descriptors) {
      snprintf(track,
               kBufferSize,
               "<TRACE> count: %llu, allocated: %llu\n%s",
               (long long unsigned int)item->count,
               (long long unsigned int)item->alloc_size,
               item->stack);
      printf(track);
    }
    fflush(stdout);
  }
}

  void operator delete(void *ptr, std::size_t) {
    makeDelete(ptr);
  }

  void operator delete(void *ptr) {
    makeDelete(ptr);
  }

void *operator new(size_t size) {
  auto const ptr = malloc(size);
  if (table_ready.load()) {
    std::lock_guard lock(tables_cs);
    static constexpr size_t kStackSize = 10;

    void *a[kStackSize];
    auto const s = backtrace(a, kStackSize);
    char **names = backtrace_symbols(a, s);

    int pos = 0;
    for (int ix = 0; ix < s; ++ix)
      pos += snprintf(track + pos, kBufferSize, "%s\n", names[ix]);
    free(names);

    auto const hash = std::_Hash_impl::hash(track, pos);
    {
      auto &entry = allocationsTable()[hash];
      if (!entry.stack[0])
        std::memcpy(entry.stack, track, pos + 1ull);
      ++entry.count;
      entry.alloc_size += malloc_usable_size(ptr);
    }
    { pointersTable()[uintptr_t(ptr)] = hash; }
  }
  return ptr;
}
