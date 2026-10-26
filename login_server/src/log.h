#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

// hb::log per Standards sect. 13. Written here first; it moves to the shared hb::shared library
// when the World server needs it.
namespace hb::log
{
	enum class level
	{
		debug,
		info,
		warning,
		error
	};

	// First line of main(): opens <application>.log (truncated) and installs the terminate handler.
	void initialize(std::string_view application, std::string_view component);
	void shutdown();

	void set_level(level minimum);
	bool parse_level(std::string_view text, level& result);

	// Writes runtime_error.log and logs the crash as an error.
	void report_crash(std::string_view reason);

	void write(level message_level, std::string_view message);

	template <typename... T>
	void debug(std::format_string<T...> format, T&&... args)
	{
		write(level::debug, std::format(format, std::forward<T>(args)...));
	}

	template <typename... T>
	void info(std::format_string<T...> format, T&&... args)
	{
		write(level::info, std::format(format, std::forward<T>(args)...));
	}

	template <typename... T>
	void warning(std::format_string<T...> format, T&&... args)
	{
		write(level::warning, std::format(format, std::forward<T>(args)...));
	}

	template <typename... T>
	void error(std::format_string<T...> format, T&&... args)
	{
		write(level::error, std::format(format, std::forward<T>(args)...));
	}
}
