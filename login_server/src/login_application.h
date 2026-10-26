#pragma once

#include "config.h"

#include <string>
#include <string_view>
#include <vector>

namespace hb::server::login
{
	constexpr std::string_view application_name = "login_server";
	constexpr std::string_view default_config_path = "login_server.json";

	class login_application
	{
	public:
		// False when the server could not start (a bad config); main maps it to the exit code.
		bool run(const std::vector<std::string>& arguments);

	private:
		void log_config() const;

		login_config m_config;
	};
}
