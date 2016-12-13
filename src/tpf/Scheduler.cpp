#include "SyncTask.h"
#include "Scheduler.h"
#include "SyncBasedOnWindowsEvent.h"

using namespace Parallel;

std::mutex Scheduler::_lock;
Scheduler* Scheduler::_scheduler;

Scheduler::Scheduler(int queuesSize)
	: _cores(std::thread::hardware_concurrency())
	, _ioQueue(new IoQueueBasedOnWindowsCompletionPorts())
	, _syncForWorkers(new SyncBasedOnWindowsEvent())
	, _ioWorkers(std::vector<IoWorker*>())
	, _cpuWorkers(std::vector<CpuWorker*>())
{
	w_contexts_by_thread_number_.reserve( _cores );
	w_contexts_by_thread_id_.reserve( _cores );

	_cpuWorkers.reserve( _cores );

	for (int i = 0; i < _cores; i++) {

		/*
		 * TODO: replace this code on init internal::w_context for all workers and add these
		 * to the w_contexts_by_thread_id_ and w_contexts_by_thread_number_ where number is "i"
		 */

		QueuesContainer* queues = new QueuesContainer( queuesSize );
		CpuWorker* worker = new CpuWorker(this, _syncForWorkers);
		_queues.insert(make_pair(worker->ThreadId(), queues));
		_cpuWorkers.push_back(worker);
		_currentTasks.insert(make_pair(worker->ThreadId(), new CurrentTask()));
	}

	SyncBasedOnWindowsEvent* syncForIoWorkers = new SyncBasedOnWindowsEvent();
	_ioWorkers.push_back(new IoWorker(*_ioQueue, syncForIoWorkers));
	syncForIoWorkers->NotifyAll();
}

internal::w_context* internal::fetch_worker_context(std::thread::id thread_id) {
	return _w_contexts_by_thread_id[thread_id];
}

void Scheduler::Spawn(Task* task) {
	if (_currentWorker.load(std::memory_order_acquire) == _cores - 1) {
		_currentWorker.store(0, std::memory_order_release);
	} else {
		++_currentWorker;
	}

	if (!_queues[_cpuWorkers[_currentWorker]->ThreadId()]->ExternalQueue.TryEnqueue(task)) {
		throw std::exception("Queue is full.");
	}

	_syncForWorkers->NotifyAll();
}

void Scheduler::Spawn(Task* task, std::thread::id threadId) {
	std::unordered_map<std::thread::id, QueuesContainer*>::const_iterator i = _queues.find(threadId);

	if (i == _queues.end()) {
		Spawn(task);
	} else {
		if (!i->second->InternalQueue.TryEnqueue(task)) {
			throw std::exception("Queue is full.");
		}

		_syncForWorkers->NotifyAll();
	}
}

void Scheduler::Wait(Task* task) {
	SyncBasedOnWindowsEvent sync;
	SyncTask* syncTask = new SyncTask(&sync);
	task->Continuation(syncTask);
	syncTask->PendingCount(1);
	Spawn(task);
	sync.Wait();
}

Task* Scheduler::FetchTaskFromLocalQueues(std::thread::id currentThreadId) {
	Task* task;
	if (_queues[currentThreadId]->InternalQueue.TryDequeue(task) || _queues[currentThreadId]->ExternalQueue.TryDequeue(task)) {
		return task;
	}
	return nullptr;
}

Task* Scheduler::StealTaskFromInternalQueueOtherWorkers(std::thread::id currentThreadId) {
	return StealTaskFromOtherWorkers([&](std::thread::id id) { return &_queues[id]->InternalQueue; }, currentThreadId);
}

Task* Scheduler::StealTaskFromExternalQueueOtherWorkers(std::thread::id currentThreadId) {
	return StealTaskFromOtherWorkers([&](std::thread::id id) { return &_queues[id]->ExternalQueue; }, currentThreadId);
}

Task* Scheduler::StealTaskFromOtherWorkers(std::function<ConcurrentQueue<Task*>*(std::thread::id)> queue, std::thread::id currentThreadId) {
	Task* task;
	for (std::unordered_map<std::thread::id, QueuesContainer*>::iterator it = _queues.begin(); it != _queues.end(); ++it) {
		if (it->first != currentThreadId) {
			if (queue( it->first )->TryDequeue( task )) {
				return task;
			}
		}
	}

	return nullptr;
}

void Scheduler::Compute(Task* task) {
	Task* continuation = nullptr;
	std::thread::id currentThreadId = std::this_thread::get_id();

	do {
		if (!task->IsCanceled()) {
			CurrentTask* current = _currentTasks[currentThreadId];
			current->Task = task;
			current->IsRecyclable = false;

			Task* c = task->Compute();

			if (!_currentTasks[currentThreadId]->IsRecyclable && task->PendingCount() == 0) {
				if (task->Continuation() != nullptr) {
					continuation = task->Continuation();
				}

				delete task;
			} else if (c == nullptr) {
				break;
			}

			if (c != nullptr) {
				task = c;
				continue;
			}
		} else {
			continuation = task->Continuation();
			delete task;
		}

		task = continuation != nullptr && continuation->DecrementPendingCount() <= 0 ? continuation : nullptr;
		continuation = nullptr;
	} while (task != nullptr);
}

task& internal::allocate_continuation_proxy::allocate(size_t size) const {
	task& t = *((task*)this);
	internal::fetch_worker_context(std::this_thread::get_id())->continuation_task = t.continuation();
	t.set_null_continuation();
	return *((task*)::operator new(size));
}

task& internal::allocate_child_proxy::allocate(size_t size) const {
	task& t = *((task*)this);
	internal::fetch_worker_context(std::this_thread::get_id())->continuation_task = &t;
	return *((task*)::operator new(size));
}

task::task() : cancellation_token_(nullptr), ref_count_(0) {
	internal::w_context* wc = internal::fetch_worker_context(std::this_thread::get_id());

	if (wc != nullptr && wc->continuation_task != nullptr) {
		continuation_ = wc->continuation_task;
		wc->continuation_task = nullptr;
	} else {
		continuation_ = nullptr;
	}
}

void task::spawn(task& t) {
	internal::spawn(t, std::this_thread::get_id());
}

void task::recycle() {
	internal::fetch_worker_context(std::this_thread::get_id())->is_recyclable = true;
}

task* task::self() {
	return internal::fetch_worker_context(std::this_thread::get_id())->current_executebale_task;
}