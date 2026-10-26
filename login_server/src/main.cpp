#include "log.h"
#include "login_application.h"

#include <exception>
#include <span>
#include <string>
#include <vector>

int main(int argc, char* argv[])
{
	hb::log::initialize(hb::server::login::application_name, "login");
	bool started = false;
	try
	{
		const std::span<char*> raw_arguments(argv, static_cast<std::size_t>(argc));
		const std::vector<std::string> arguments(raw_arguments.begin(), raw_arguments.end());
		hb::server::login::login_application application;
		started = application.run(arguments);
	}
	catch (const std::exception& error)
	{
		hb::log::report_crash(error.what());
	}
	hb::log::shutdown();
	return started ? 0 : 1;
}
