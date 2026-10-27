#pragma once

#include "common_types.h"

// Enter-game session tickets (plan sect. 4.2). A ticket is 32 bytes from the operating system's CSPRNG
// (OpenSSL RAND_bytes), sent to the World in session_grant and to the client in enter_game_response, and
// never logged. The Login server keeps no copy once both are sent: the World consumes the ticket once and
// compares it in constant time (CRYPTO_memcmp or equivalent), the Login server never compares one.
namespace hb::server::login
{
	// Fills the ticket with random bytes. False (the ticket zeroed) when the CSPRNG fails.
	bool generate_ticket(hb::net::session_ticket& ticket);

	// Overwrites the ticket in a way the optimiser may not remove.
	void wipe_ticket(hb::net::session_ticket& ticket);

	// True when the ticket is all zero (never issued, or wiped).
	bool is_blank(const hb::net::session_ticket& ticket);
}
