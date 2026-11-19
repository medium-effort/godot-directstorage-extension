#include "gdeflate_parallel.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>

#include <atomic>

namespace godot {

namespace {

struct JobBridge {
	void (*job)(void *, uint32_t);
	void *user;
};

// Pool capacity as resolved at module init. Guarded by nothing on purpose: it is
// written once on the main thread before any load can dispatch a job, and only read
// afterwards.
int g_pool_threads = 1;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
std::atomic<uint32_t> g_last_parallel_workers{ 0 };
#endif

void run_element(void *p_user, uint32_t p_index) {
	JobBridge *bridge = static_cast<JobBridge *>(p_user);
	bridge->job(bridge->user, p_index);
}

} // namespace

void ds_gdeflate_init_pool_capacity() {
	// threading/worker_pool/max_threads: -1 means "all logical CPU cores" and may be
	// set to 1, which is the only configuration that can leave a waiting caller with no
	// worker able to run its job.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const int64_t configured = ps
			? static_cast<int64_t>(ps->get_setting("threading/worker_pool/max_threads", -1))
			: -1;
	const OS *os = OS::get_singleton();
	const int64_t processors = os ? static_cast<int64_t>(os->get_processor_count()) : 1;
	const int64_t resolved = configured > 0 ? configured : (processors > 0 ? processors : 1);
	g_pool_threads = static_cast<int>(resolved);
}

int ds_gdeflate_get_pool_capacity() {
	return g_pool_threads;
}

void ds_gdeflate_parallel_for(void *p_user, void (*p_job)(void *, uint32_t), uint32_t p_workers) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	g_last_parallel_workers.store(p_workers);
#endif
	if (p_job == nullptr) {
		return;
	}

	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	// Two usable workers are the minimum that can still make progress while one is
	// blocked elsewhere. The pool does not expose its real worker count and creates
	// fewer threads than the configured maximum, so a configured size below 3 is
	// treated as single-worker and the job runs on the calling thread.
	if (p_workers <= 1 || pool == nullptr || g_pool_threads < 3) {
		p_job(p_user, 0);
		return;
	}

	// The bridge lives on this stack frame, and the wait below does not return until
	// every element has finished, so the pointer stays valid for all of them.
	JobBridge bridge{ p_job, p_user };
	const int64_t group = pool->add_native_group_task(&run_element, &bridge,
			static_cast<int32_t>(p_workers), static_cast<int32_t>(p_workers), false, "DS GDeflate");
	if (group == static_cast<int64_t>(WorkerThreadPool::INVALID_TASK_ID)) {
		p_job(p_user, 0);
		return;
	}
	pool->wait_for_group_task_completion(group);
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
uint32_t ds_get_last_parallel_for_workers_for_testing() {
	return g_last_parallel_workers.load();
}

void ds_reset_parallel_for_workers_for_testing() {
	g_last_parallel_workers.store(0);
}
#endif

} // namespace godot
