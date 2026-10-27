#pragma once

#include "asio_config.h"
#include "log.h"

#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

// Worker threads for slow jobs (password hashing, Schemata calls; plan sect. 5.1), sized to the Schemata
// pool so a job never waits on a connection. The network io_context stays on the main thread; jobs are
// posted here and post their results back to the session's strand. Each thread installs the crash handler.
namespace hb::server::login
{
	class worker_pool
	{
	public:
		explicit worker_pool(std::size_t threads)
			: m_guard(asio::make_work_guard(m_jobs))
		{
			for (std::size_t i = 0; i < threads; ++i)
			{
				m_threads.emplace_back([this] {
					log::install_thread_crash_handler();
					m_jobs.run();
				});
			}
		}

		worker_pool(const worker_pool&) = delete;
		worker_pool& operator=(const worker_pool&) = delete;

		~worker_pool()
		{
			stop();
		}

		template <class Job>
		void post(Job&& job)
		{
			asio::post(m_jobs, std::forward<Job>(job));
		}

		// Lets queued jobs finish, then joins the threads.
		void stop()
		{
			m_guard.reset();
			m_threads.clear();  // jthread joins
		}

		std::size_t size() const { return m_threads.size(); }

	private:
		asio::io_context m_jobs;
		asio::executor_work_guard<asio::io_context::executor_type> m_guard;
		std::vector<std::jthread> m_threads;
	};
}
