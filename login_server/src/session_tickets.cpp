#include "session_tickets.h"

#include "log.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace hb::server::login
{
	bool generate_ticket(hb::net::session_ticket& ticket)
	{
		if (RAND_bytes(ticket.data(), static_cast<int>(ticket.size())) == 1)
			return true;
		log::error("RAND_bytes failed");
		wipe_ticket(ticket);
		return false;
	}

	void wipe_ticket(hb::net::session_ticket& ticket)
	{
		OPENSSL_cleanse(ticket.data(), ticket.size());
	}

	bool is_blank(const hb::net::session_ticket& ticket)
	{
		std::uint8_t any = 0;
		for (const std::uint8_t byte : ticket)
			any |= byte;
		return any == 0;
	}
}
