#pragma once

#include <thread>
#include <atomic>

#if defined(_WIN32)
//---------------------------------------------------------
// Semaphore (Windows)
//---------------------------------------------------------
#include <windows.h>
#undef min
#undef max
#elif defined(__MACH__)
//---------------------------------------------------------
// Semaphore (Apple iOS and OSX)
// Can't use POSIX semaphores due to http://lists.apple.com/archives/darwin-kernel/2009/Apr/msg00010.html
//---------------------------------------------------------
#include <mach/mach.h>
#elif defined(__unix__)
//---------------------------------------------------------
// Semaphore (POSIX, Linux)
//---------------------------------------------------------
#include <semaphore.h>
#else
#error Unsupported platform!
#endif

class silk__slim_semaphore {
	std::atomic<int> count_;
#if defined(_WIN32)
	HANDLE s_;
#elif defined(__MACH__)
	semaphore_t s_;
#elif defined(__unix__)
	sem_t s_;
#endif
public:
	silk__slim_semaphore(int initialCount = 0) : count_(initialCount) {
#if defined(_WIN32)
		s_ = CreateSemaphore(NULL, initialCount, MAXLONG, NULL);
#elif defined(__MACH__)
		semaphore_create(mach_task_self(), &s_, SYNC_POLICY_FIFO, initialCount);
#elif defined(__unix__)
		sem_init(&s_, 0, initialCount);
#endif	
	}

	~silk__slim_semaphore() {
#if defined(_WIN32)
		CloseHandle(s_);
#elif defined(__MACH__)
		semaphore_destroy(mach_task_self(), s_);
#elif defined(__unix__)
		sem_destroy(&s_);
#endif
	}

	void wait() {
		int oldCount = count_.load(std::memory_order_relaxed);

		if ((oldCount > 0 && count_.compare_exchange_strong(oldCount, oldCount - 1, std::memory_order_acquire)))
			return;
		
		int spin = 10000;

		while (spin--) {
			oldCount = count_.load(std::memory_order_relaxed);

			if ((oldCount > 0) && count_.compare_exchange_strong(oldCount, oldCount - 1, std::memory_order_acquire))
				return;
			
			std::atomic_signal_fence(std::memory_order_acquire);
		}

		oldCount = count_.fetch_sub(1, std::memory_order_acquire);
		
		if (oldCount <= 0) {
#if defined(_WIN32)
			WaitForSingleObject(s_, INFINITE);
#elif defined(__MACH__)
			semaphore_wait(s_);
#elif defined(__unix__)
			int rc;
			do {
				rc = sem_wait(&s_);
			} while (rc == -1 && errno == EINTR);
#endif
		}
	}

	void signal(const int count = 1) {
		const int old_count = count_.fetch_add(count, std::memory_order_release);
		int to_release = -old_count < count ? -old_count : count;

		if (to_release > 0) {
#if defined(_WIN32)
			ReleaseSemaphore(s_, to_release, NULL);
#elif defined(__MACH__)
			while (to_release-- > 0) {
				semaphore_signal(s_);
			}
#elif defined(__unix__)
			while (to_release-- > 0) {
				sem_post(&s_);
			}
#endif
		}
	}
};

class silk__auto_reset_event {
	std::atomic<int> m_status_;
	silk__slim_semaphore m_sema_;

public:
	silk__auto_reset_event() : m_status_(0) {
	}

	void signal(const int count = 1) {
		int old_status = m_status_.load(std::memory_order_relaxed);
		for (;;) {
		
			const int new_status = old_status < 1 ? old_status + 1 : 1;
			
			if (m_status_.compare_exchange_weak(old_status, new_status, std::memory_order_release, std::memory_order_relaxed))
				break;
		}

		if (old_status < 0)
			m_sema_.signal(count);
	}

	void wait() {
		const int old_status = m_status_.fetch_sub(1, std::memory_order_acquire);

		if (old_status < 1) {
			m_sema_.wait();
		}
	}
};

template<int> struct int_to_type {};

class silk__fast_random {
	unsigned x, c;
	static const unsigned a = 0x9e3779b1; // a big prime number
public:
	unsigned short get() {
		return get(x);
	}

	unsigned short get(unsigned& seed) {
		unsigned short r = (unsigned short)(seed >> 16);
		seed = seed*a + c;
		return r;
	}

	silk__fast_random(void* unique_ptr) { init(uintptr_t(unique_ptr)); }
	silk__fast_random(uint32_t seed) { init(seed); }
	silk__fast_random(uint64_t seed) { init(seed); }

	template <typename T>
	void init(T seed) {
		init(seed, int_to_type<sizeof(seed)>());
	}

	void init(uint64_t seed, int_to_type<8>) {
		init(uint32_t((seed >> 32) + seed), int_to_type<4>());
	}

	void init(uint32_t seed, int_to_type<4>) {
		// threads use different seeds for unique sequences
		c = (seed | 1) * 0xba5703f5; // c must be odd, shuffle by a prime number
		x = c ^ (seed >> 1); // also shuffle x for the first get() invocation
	}
};

class silk__spin_lock {
	std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
public:
	void lock() {
		while (lock_.test_and_set(std::memory_order_acquire)) {
		}
	}

	void unlock() {
		lock_.clear(std::memory_order_release);
	}
};

typedef struct silk__task_t {
	silk__task_t* next;
	silk__task_t* prev;
} silk__task;

typedef struct silk__wcontext_t {
	silk__fast_random* random;
	silk__spin_lock* affinity_sync;
	silk__task* affinity_tail;
	silk__task* affinity_head;
	silk__spin_lock* sync;
	silk__task* tail;
	silk__task* head;
} silk__wcontext;

int silk__workers_count;
silk__wcontext** silk__wcontexts;
silk__auto_reset_event* silk__sem = new silk__auto_reset_event();

inline void silk__spawn(const int worker_id, silk__task* t) {
	silk__wcontext* c = silk__wcontexts[worker_id];

	t->prev = t->next = nullptr;
    
	c->sync->lock();

	if (!c->head) {
		c->tail = c->head = t;
	} else {
		t->next = c->head;
		c->head->prev = t;
		c->head = t;
	}

	c->sync->unlock();

	silk__sem->signal(silk__workers_count);
}

inline silk__task* silk__fetch(const int worker_id) {
	silk__wcontext* c = silk__wcontexts[worker_id];

	silk__task* t = nullptr;

	c->sync->lock();

	if (c->head) {
		t = c->head;
		
		c->head = t->next;

		if (!c->head)
			c->tail = nullptr;
		else
			c->head->prev = nullptr;

		t->prev = t->next = nullptr;
	}

	c->sync->unlock();

	return t;
}

inline silk__task* silk__steal(const int thief_thread_id) {
	silk__task* t = nullptr;

	silk__wcontext* c = silk__wcontexts[thief_thread_id];

	for (int i = 0; i < 100; i++) {
		int v = c->random->get() % silk__workers_count;

		if (v == thief_thread_id)
			continue;

		silk__wcontext* vc = silk__wcontexts[v];

		vc->sync->lock();

		if (vc->head) {
			t = vc->tail;
			vc->tail = t->prev;

			if (!vc->tail)
				vc->head = nullptr;
			else
				vc->tail->next = nullptr;
			
			t->prev = t->next = nullptr;
		}

		vc->sync->unlock();

		if (t)
			return t;
	}

	return t;
}

void silk__enqueue( const int worker_id, silk__task* t ) {
	silk__wcontext* c = silk__wcontexts[worker_id];

	t->prev = t->next = nullptr;
	
	c->sync->lock();

	if (!c->head) {
		c->tail = c->head = t;
	} else {
		t->next = c->head;
		c->head->prev = t;
		c->head = t;
	}

	c->sync->unlock();
 
    silk__sem->signal(silk__workers_count);
}

inline void silk__spawn_affinity(const int worker_id, silk__task* t) {
	silk__wcontext* c = silk__wcontexts[worker_id];

	t->prev = t->next = nullptr;
    
	c->affinity_sync->lock();

	if (!c->affinity_head) {
		c->affinity_tail = c->affinity_head = t;
	} else {
		t->next = c->affinity_head;
		c->affinity_head->prev = t;
		c->affinity_head = t;
	}

	c->affinity_sync->unlock();

	silk__sem->signal(silk__workers_count);
}

void silk__enqueue_affinity( const int worker_id, silk__task* t ) {
	silk__wcontext* c = silk__wcontexts[worker_id];

	t->prev = t->next = nullptr;
	
	c->affinity_sync->lock();

	if (!c->affinity_head) {
		c->affinity_tail = c->affinity_head = t;
	} else {
		t->next = c->affinity_head;
		c->affinity_head->prev = t;
		c->affinity_head = t;
	}

	c->affinity_sync->unlock();
 
    silk__sem->signal(silk__workers_count);
}

inline silk__task* silk__fetch_affinity( const int worker_id ) {
    silk__wcontext* c = silk__wcontexts[worker_id];
 
    silk__task* t = nullptr;
 
    c->affinity_sync->lock();

	if (c->affinity_head) {
		t = c->affinity_tail;

		c->affinity_tail = t->prev;

		if (!c->affinity_tail)
			c->affinity_head = nullptr;
		else 
			c->affinity_tail->next = nullptr;
	}
 
    c->affinity_sync->unlock();
 
    return t;
}