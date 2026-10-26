#include "log.h"

#include "console.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>

// The sink state stays private to this file: it drags <fstream> and <mutex> (sect. 4 exception).
namespace hb::log
{
	class sink
	{
	public:
		static sink& instance()
		{
			static sink single;
			return single;
		}

		void open(std::string_view application, std::string_view component)
		{
			std::scoped_lock lock(m_mutex);
			m_component = component;
			m_file.open(std::string(application) + ".log", std::ios::out | std::ios::trunc | std::ios::binary);
			m_colors = hb::server::platform::stdout_is_terminal() && hb::server::platform::enable_console_colors();
			if (!m_file.is_open())
				std::fputs(std::format("cannot open {}.log; logging to the console only\n", application).c_str(), stderr);
		}

		void close()
		{
			std::scoped_lock lock(m_mutex);
			m_file.close();
		}

		void set_minimum(level minimum)
		{
			m_minimum.store(minimum, std::memory_order_relaxed);
		}

		void write(level message_level, std::string_view message)
		{
			if (message_level < m_minimum.load(std::memory_order_relaxed))
				return;
			const std::string_view tag = level_tags[static_cast<std::size_t>(message_level)];
			std::scoped_lock lock(m_mutex);
			const auto now = local_now();
			if (m_file.is_open())
			{
				m_file << std::format("{:%Y-%m-%d %T} [{}] {}: {}\r\n", now, tag, m_component, message);
				m_file.flush();
			}
			const std::string_view color = m_colors ? level_colors[static_cast<std::size_t>(message_level)] : "";
			const std::string_view reset = m_colors ? "\x1b[0m" : "";
			std::fputs(std::format("{:%T} {}[{}]{} {}\n", now, color, tag, reset, message).c_str(), stdout);
			std::fflush(stdout);
		}

		// Writes runtime_error.log, then logs the crash.
		void report_crash(std::string_view reason)
		{
			{
				std::ofstream file("runtime_error.log", std::ios::out | std::ios::trunc | std::ios::binary);
				file << std::format("{:%Y-%m-%d %T}\r\n{}\r\n", local_now(), reason);
			}
			write(level::error, std::format("crash: {}", reason));
		}

		static void on_terminate()
		{
			std::string reason = "std::terminate called with no active exception";
			if (const std::exception_ptr current = std::current_exception())
			{
				try
				{
					std::rethrow_exception(current);
				}
				catch (const std::exception& caught)
				{
					reason = std::string("unhandled exception: ") + caught.what();
				}
				catch (...)
				{
					reason = "unhandled non-standard exception";
				}
			}
			instance().report_crash(reason);
			// _Exit, not abort: a Debug abort waits on a message box and hangs a headless run.
			std::_Exit(crash_exit_code);
		}

	private:
		static constexpr int crash_exit_code = 3;
		static constexpr std::array<std::string_view, 4> level_tags = { "DEBUG", "INFO", "WARN", "ERROR" };
		static constexpr std::array<std::string_view, 4> level_colors = { "\x1b[90m", "\x1b[0m", "\x1b[33m", "\x1b[31m" };

		static auto local_now()
		{
			return std::chrono::zoned_time(std::chrono::current_zone(),
				std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
		}

		std::mutex m_mutex;
		std::ofstream m_file;
		std::string m_component;
		std::atomic<level> m_minimum = level::info;
		bool m_colors = false;
	};

	void initialize(std::string_view application, std::string_view component)
	{
		sink::instance().open(application, component);
		std::set_terminate(sink::on_terminate);
	}

	void shutdown()
	{
		sink::instance().close();
	}

	void set_level(level minimum)
	{
		sink::instance().set_minimum(minimum);
	}

	bool parse_level(std::string_view text, level& result)
	{
		static constexpr std::array<std::pair<std::string_view, level>, 4> names = { {
			{ "debug", level::debug },
			{ "info", level::info },
			{ "warning", level::warning },
			{ "error", level::error },
		} };
		for (const auto& [name, value] : names)
		{
			if (name == text)
			{
				result = value;
				return true;
			}
		}
		return false;
	}

	void report_crash(std::string_view reason)
	{
		sink::instance().report_crash(reason);
	}

	void write(level message_level, std::string_view message)
	{
		sink::instance().write(message_level, message);
	}
}
