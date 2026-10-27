#include "authenticator.h"

#include "asio_config.h"
#include "character_rules.h"
#include "link_messages.h"
#include "log.h"
#include "login_messages.h"
#include "message_codec.h"
#include "message_id.h"
#include "session_tickets.h"

#include <openssl/crypto.h>

#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hb::server::login
{
	namespace
	{
		namespace proto = hb::net::login;
		using hb::net::message_id;

		// Passwords are wiped from every copy this file makes. OPENSSL_cleanse is not optimised away.
		void wipe(std::string& text)
		{
			if (!text.empty())
				OPENSSL_cleanse(text.data(), text.size());
			text.clear();
		}

		template <class T>
		void wipe(T& message)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			OPENSSL_cleanse(&message, sizeof(T));
		}

		proto::account_result to_account_result(create_result result)
		{
			switch (result)
			{
			case create_result::created:
				return proto::account_result::ok;
			case create_result::invalid_name:
				return proto::account_result::invalid_name;
			case create_result::name_taken:
				return proto::account_result::name_taken;
			case create_result::invalid_password:
			case create_result::failed:
				break;
			}
			return proto::account_result::failed;
		}
	}

	authenticator_options make_authenticator_options(const login_config& config)
	{
		authenticator_options options;
		options.allow_account_creation = config.accounts.allows_client_account_creation;
		options.idle_after_login = std::chrono::seconds(config.session.idle_timeout_after_login_seconds);
		options.max_body_bytes = config.protocol.max_body_bytes;
		return options;
	}

	authenticator::authenticator(account_service& accounts, worker_pool& workers, world_link_service& worlds, authenticator_options options)
		: m_accounts(accounts)
		, m_workers(workers)
		, m_worlds(worlds)
		, m_options(options)
	{
	}

	void authenticator::handle(hb::net::connection& from, const hb::net::frame& received)
	{
		const std::shared_ptr<hb::net::connection> session = from.shared_from_this();
		const std::uint16_t id = received.header.message_id;
		if (id == hb::net::to_wire(message_id::login_request))
			on_login(session, received);
		else if (id == hb::net::to_wire(message_id::create_account_request))
			on_create_account(session, received);
		else if (id == hb::net::to_wire(message_id::change_password_request))
			on_change_password(session, received);
		else if (!is_authenticated(from))
			from.close(hb::net::close_reason::protocol_error, std::format("message 0x{:04x} before login", id));
		else if (id == hb::net::to_wire(message_id::create_character_request))
			on_create_character(session, received);
		else if (id == hb::net::to_wire(message_id::delete_character_request))
			on_delete_character(session, received);
		else if (id == hb::net::to_wire(message_id::enter_game_request))
			on_enter_game(session, received);
		else
			log::debug("client {}: message 0x{:04x} not handled yet", from.remote_address(), id);
	}

	void authenticator::on_closed(const hb::net::connection& session)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found == m_sessions.end())
			return;
		wipe_ticket(found->second.ticket);
		if (found->second.authenticated)
		{
			// Only the account's current session clears the entry; a superseded one must not remove its successor.
			const auto live = m_live.find(found->second.account_id);
			if (live != m_live.end())
			{
				const std::shared_ptr<hb::net::connection> current = live->second.lock();
				if (!current || current.get() == &session)
					m_live.erase(live);
			}
		}
		m_sessions.erase(found);
	}

	bool authenticator::is_authenticated(const hb::net::connection& session) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		return found != m_sessions.end() && found->second.authenticated;
	}

	std::size_t authenticator::authenticated_count() const
	{
		std::scoped_lock lock(m_mutex);
		return m_live.size();
	}

	bool authenticator::begin_request(const std::shared_ptr<hb::net::connection>& session)
	{
		bool refused = false;
		{
			std::scoped_lock lock(m_mutex);
			session_state& state = m_sessions[session.get()];
			refused = state.busy;
			state.busy = true;
		}
		if (refused)
			session->close(hb::net::close_reason::protocol_error, "request while another is pending");
		return !refused;
	}

	bool authenticator::count_failure(hb::net::connection& session)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found == m_sessions.end())
			return true;
		return ++found->second.failures >= m_options.max_failed_attempts;
	}

	void authenticator::on_login(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::login_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed login_request");
			return;
		}
		std::string name(request.account.view());
		std::string password(request.password.view());
		std::string world(request.world.view());
		wipe(request);
		if (is_authenticated(*session))
		{
			wipe(password);
			session->close(hb::net::close_reason::protocol_error, "login_request on a logged-in session");
			return;
		}
		if (!begin_request(session))
		{
			wipe(password);
			return;
		}
		// The World is checked before the password: a refused world costs no Argon2 run, and a client
		// cannot log in to a World that cannot give it a character list anyway.
		if (!m_worlds.knows_world(world) || !m_worlds.is_live(world))
		{
			wipe(password);
			end_request(*session);
			const bool known = m_worlds.knows_world(world);
			// The name is echoed only when it is a configured one; a client's own text never reaches the log.
			if (known)
				log::info("login from {} refused: world {} unavailable", session->remote_address(), world);
			else
				log::info("login from {} refused: unknown world", session->remote_address());
			reply_login(*session, known ? proto::login_result::world_unavailable : proto::login_result::no_such_world);
			return;
		}
		{
			std::scoped_lock lock(m_mutex);
			m_sessions[session.get()].world = std::move(world);
		}

		m_workers.post([this, weak = std::weak_ptr<hb::net::connection>(session), name = std::move(name),
			password = std::move(password), address = session->remote_address()]() mutable {
			const login_outcome outcome = m_accounts.authenticate(name, password, address);
			wipe(password);
			const std::shared_ptr<hb::net::connection> alive = weak.lock();
			if (!alive)
				return;
			asio::post(alive->executor(), [this, alive, outcome, address = std::move(address)] {
				finish_login(*alive, outcome, address);
			});
		});
	}

	void authenticator::finish_login(hb::net::connection& session, const login_outcome& outcome, const std::string& address)
	{
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_sessions.find(&session);
			if (found == m_sessions.end())
				return;  // the session closed while the check ran
			found->second.busy = false;
		}
		if (!session.is_open())
			return;

		proto::login_response reply{};
		bool exhausted = false;
		switch (outcome.result)
		{
		case login_result::accepted:
			// Not logged in yet: the World's character list comes first, and the session stays busy until then.
			{
				std::scoped_lock lock(m_mutex);
				m_sessions[&session].busy = true;
			}
			begin_login_list(session, outcome, address);
			return;
		case login_result::no_account:
		case login_result::wrong_password:
			// One reply for both, so the answer does not say which account names exist.
			reply.result = proto::login_result::password_mismatch;
			exhausted = count_failure(session);
			log::info("login from {} refused", address);
			break;
		case login_result::locked:
			reply.result = proto::login_result::account_locked;
			log::info("login from {} refused: account locked", address);
			break;
		case login_result::failed:
			reply.result = proto::login_result::service_unavailable;
			// An unusable row or missing pepper is still an attempt against that account.
			exhausted = count_failure(session);
			break;
		}
		(void)session.send(reply);
		if (exhausted)
			session.close_after_send(hb::net::close_reason::protocol_error, "too many failed attempts");
	}

	void authenticator::audit_refusal(const std::shared_ptr<hb::net::connection>& session, std::string name, std::string_view outcome)
	{
		// The audit row is a database write, so it goes to a worker; the reply does not wait for it.
		m_workers.post([this, name = std::move(name), address = session->remote_address(), outcome] {
			m_accounts.audit(name, address, outcome);
		});
	}

	void authenticator::on_create_account(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::create_account_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed create_account_request");
			return;
		}
		std::string name(request.account.view());
		std::string password(request.password.view());
		wipe(request);
		if (!m_options.allow_account_creation)
		{
			wipe(password);
			audit_refusal(session, std::move(name), "create_disabled");
			(void)session->send(proto::create_account_response{ proto::account_result::disabled });
			return;
		}

		bool exhausted = false;
		{
			std::scoped_lock lock(m_mutex);
			exhausted = ++m_sessions[session.get()].creations > m_options.max_account_creations;
		}
		if (exhausted)
		{
			wipe(password);
			audit_refusal(session, std::move(name), "create_capped");
			session->close(hb::net::close_reason::protocol_error, "too many account creations");
			return;
		}
		if (!begin_request(session))
		{
			wipe(password);
			return;
		}

		m_workers.post([this, weak = std::weak_ptr<hb::net::connection>(session), name = std::move(name),
			password = std::move(password), address = session->remote_address()]() mutable {
			const create_result result = m_accounts.create(name, password, address);
			wipe(password);
			const std::shared_ptr<hb::net::connection> alive = weak.lock();
			if (!alive)
				return;
			asio::post(alive->executor(), [this, alive, result] { finish_create(*alive, result); });
		});
	}

	void authenticator::finish_create(hb::net::connection& session, create_result result)
	{
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_sessions.find(&session);
			if (found == m_sessions.end())
				return;
			found->second.busy = false;
		}
		if (session.is_open())
			(void)session.send(proto::create_account_response{ to_account_result(result) });
	}

	void authenticator::on_change_password(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::change_password_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed change_password_request");
			return;
		}
		std::string name(request.account.view());
		std::string old_password(request.old_password.view());
		std::string new_password(request.new_password.view());
		wipe(request);
		if (!begin_request(session))
		{
			wipe(old_password);
			wipe(new_password);
			return;
		}

		m_workers.post([this, weak = std::weak_ptr<hb::net::connection>(session), name = std::move(name),
			old_password = std::move(old_password), new_password = std::move(new_password),
			address = session->remote_address()]() mutable {
			const password_change_result result = m_accounts.change_password(name, old_password, new_password, address);
			wipe(old_password);
			wipe(new_password);
			const std::shared_ptr<hb::net::connection> alive = weak.lock();
			if (!alive)
				return;
			asio::post(alive->executor(), [this, alive, result] { finish_change(*alive, result); });
		});
	}

	void authenticator::finish_change(hb::net::connection& session, password_change_result result)
	{
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_sessions.find(&session);
			if (found == m_sessions.end())
				return;
			found->second.busy = false;
		}
		if (!session.is_open())
			return;
		// Wrong old password, locked account, unusable row and a bad new password all answer "failed".
		const bool changed = result == password_change_result::changed;
		const bool exhausted = result == password_change_result::wrong_credentials && count_failure(session);
		(void)session.send(proto::change_password_response{ changed ? proto::account_result::ok : proto::account_result::failed });
		if (exhausted)
			session.close_after_send(hb::net::close_reason::protocol_error, "too many failed attempts");
	}

	bool authenticator::session_target(const hb::net::connection& session, std::uint64_t& account_id, std::string& world) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found == m_sessions.end())
			return false;
		account_id = static_cast<std::uint64_t>(found->second.account_id);
		world = found->second.world;
		return true;
	}

	void authenticator::end_request(const hb::net::connection& session)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found != m_sessions.end())
			found->second.busy = false;
	}

	void authenticator::reply_login(hb::net::connection& session, proto::login_result result)
	{
		proto::login_response reply{};
		reply.result = result;
		(void)session.send(reply);
	}

	bool authenticator::encode_list(const character_list_reply& list, std::vector<std::byte>& bytes) const
	{
		const proto::character_list head{ list.max_slots, static_cast<std::uint8_t>(list.characters.size()) };
		return hb::net::encode_character_list(head, list.characters, m_options.max_body_bytes, bytes);
	}

	void authenticator::begin_login_list(hb::net::connection& session, const login_outcome& outcome, const std::string& address)
	{
		std::uint64_t unused_account = 0;
		std::string world;
		if (!session_target(session, unused_account, world))
			return;
		m_worlds.request_character_list(world, static_cast<std::uint64_t>(outcome.account_id), session.executor(),
			[this, weak = std::weak_ptr<hb::net::connection>(session.shared_from_this()), outcome, address](
				world_request_status status, character_list_reply list) {
				if (const std::shared_ptr<hb::net::connection> alive = weak.lock())
					finish_login_list(alive, outcome, address, status, std::move(list));
			});
	}

	void authenticator::finish_login_list(const std::shared_ptr<hb::net::connection>& session, const login_outcome& outcome,
		const std::string& address, world_request_status status, character_list_reply list)
	{
		end_request(*session);
		if (!session->is_open())
			return;
		if (status != world_request_status::answered)
		{
			log::info("login of account {} from {} refused: the World did not give the character list", outcome.name, address);
			reply_login(*session, proto::login_result::world_unavailable);
			return;
		}
		std::vector<std::byte> list_bytes;
		if (!encode_list(list, list_bytes))
		{
			log::error("login of account {}: the character list does not fit a frame", outcome.name);
			reply_login(*session, proto::login_result::service_unavailable);
			return;
		}

		std::shared_ptr<hb::net::connection> previous;
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_sessions.find(session.get());
			if (found == m_sessions.end())
				return;
			found->second.authenticated = true;
			found->second.account_id = outcome.account_id;
			const auto live = m_live.find(outcome.account_id);
			if (live != m_live.end())
			{
				previous = live->second.lock();
				live->second = session;
			}
			else
			{
				m_live.emplace(outcome.account_id, session);
			}
		}
		if (previous && previous.get() != session.get())
		{
			log::info("account {} logged in again from {}; closing the session from {}", outcome.name, address, previous->remote_address());
			previous->close(hb::net::close_reason::local_close, "superseded by a second login");
		}
		else
		{
			log::info("account {} logged in from {}", outcome.name, address);
		}
		// Logged in: the pre-login deadline no longer applies and the longer idle timeout takes over.
		session->set_deadline(std::chrono::milliseconds(0));
		session->set_idle_timeout(m_options.idle_after_login);
		proto::login_response reply{};
		reply.result = proto::login_result::ok;
		(void)session->send(reply);
		session->send_encoded(std::move(list_bytes));
	}

	void authenticator::send_change_reply(hb::net::connection& session, change_kind kind, proto::character_result result)
	{
		if (kind == change_kind::create)
			(void)session.send(proto::create_character_response{ result });
		else
			(void)session.send(proto::delete_character_response{ result });
	}

	void authenticator::on_create_character(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::create_character_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed create_character_request");
			return;
		}
		if (!begin_request(session))
			return;
		std::uint64_t account_id = 0;
		std::string world;
		if (!session_target(*session, account_id, world))
			return;
		const proto::character_result format = check_create_format(request.name.view(), request.sex, request.skin, request.stats);
		if (format != proto::character_result::ok)
		{
			end_request(*session);
			log::debug("account {}: create_character refused before the World: result {}", account_id, static_cast<unsigned>(format));
			send_change_reply(*session, change_kind::create, format);
			return;
		}
		hb::net::link::create_character_request forward{};
		forward.account_id = account_id;
		forward.name = request.name;
		forward.sex = request.sex;
		forward.skin = request.skin;
		forward.hair_style = request.hair_style;
		forward.hair_colour = request.hair_colour;
		forward.underwear_colour = request.underwear_colour;
		forward.stats = request.stats;
		m_worlds.request_create_character(world, forward, session->executor(),
			[this, weak = std::weak_ptr<hb::net::connection>(session)](world_request_status status, proto::character_result result) {
				if (const std::shared_ptr<hb::net::connection> alive = weak.lock())
					finish_character_change(alive, change_kind::create, status, result);
			});
	}

	void authenticator::on_delete_character(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::delete_character_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed delete_character_request");
			return;
		}
		if (!begin_request(session))
			return;
		std::uint64_t account_id = 0;
		std::string world;
		if (!session_target(*session, account_id, world))
			return;
		if (!is_valid_character_name(request.name.view()))
		{
			end_request(*session);
			send_change_reply(*session, change_kind::remove, proto::character_result::invalid_name);
			return;
		}
		hb::net::link::delete_character_request forward{};
		forward.account_id = account_id;
		forward.name = request.name;
		m_worlds.request_delete_character(world, forward, session->executor(),
			[this, weak = std::weak_ptr<hb::net::connection>(session)](world_request_status status, proto::character_result result) {
				if (const std::shared_ptr<hb::net::connection> alive = weak.lock())
					finish_character_change(alive, change_kind::remove, status, result);
			});
	}

	void authenticator::finish_character_change(const std::shared_ptr<hb::net::connection>& session, change_kind kind,
		world_request_status status, proto::character_result result)
	{
		if (!session->is_open())
		{
			end_request(*session);
			return;
		}
		if (status != world_request_status::answered)
		{
			end_request(*session);
			send_change_reply(*session, kind, proto::character_result::world_unavailable);
			return;
		}
		if (result != proto::character_result::ok)
		{
			end_request(*session);
			send_change_reply(*session, kind, result);
			return;
		}
		// The change was made: the client gets the fresh list with the answer, so ask the World for it.
		std::uint64_t account_id = 0;
		std::string world;
		if (!session_target(*session, account_id, world))
			return;
		m_worlds.request_character_list(world, account_id, session->executor(),
			[this, kind, weak = std::weak_ptr<hb::net::connection>(session)](world_request_status list_status, character_list_reply list) {
				if (const std::shared_ptr<hb::net::connection> alive = weak.lock())
					finish_change_list(alive, kind, list_status, std::move(list));
			});
	}

	void authenticator::finish_change_list(const std::shared_ptr<hb::net::connection>& session, change_kind kind,
		world_request_status status, character_list_reply list)
	{
		end_request(*session);
		if (!session->is_open())
			return;
		std::vector<std::byte> list_bytes;
		if (status != world_request_status::answered || !encode_list(list, list_bytes))
		{
			// The change itself went through; the client learns the list on its next login.
			log::warning("character change for {} done, but the fresh list is unavailable", session->remote_address());
			send_change_reply(*session, kind, status == world_request_status::answered ? proto::character_result::failed
				: proto::character_result::world_unavailable);
			return;
		}
		send_change_reply(*session, kind, proto::character_result::ok);
		session->send_encoded(std::move(list_bytes));
	}

	void authenticator::reply_enter(hb::net::connection& session, proto::enter_result result)
	{
		proto::enter_game_response reply{};
		reply.result = result;
		(void)session.send(reply);
	}

	bool authenticator::take_enter_ticket(const hb::net::connection& session, hb::net::session_ticket& ticket)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found == m_sessions.end())
			return false;
		ticket = found->second.ticket;
		wipe_ticket(found->second.ticket);
		return true;
	}

	void authenticator::on_enter_game(const std::shared_ptr<hb::net::connection>& session, const hb::net::frame& received)
	{
		proto::enter_game_request request{};
		if (!hb::net::decode(received, request))
		{
			session->close(hb::net::close_reason::protocol_error, "malformed enter_game_request");
			return;
		}
		if (!begin_request(session))
			return;
		std::uint64_t account_id = 0;
		std::string world;
		if (!session_target(*session, account_id, world))
			return;
		const std::string character(request.character.view());
		if (!is_valid_character_name(character))
		{
			end_request(*session);
			reply_enter(*session, proto::enter_result::no_such_character);
			return;
		}
		if (!m_worlds.is_live(world))
		{
			end_request(*session);
			reply_enter(*session, proto::enter_result::world_unavailable);
			return;
		}

		online_registry& online = m_worlds.online();
		const enter_outcome outcome = online.begin_enter(account_id, world, character, request.kind == proto::enter_kind::force_login);
		if (outcome.decision == enter_decision::already_playing)
		{
			end_request(*session);
			log::info("account {} refused entry: already playing", account_id);
			reply_enter(*session, proto::enter_result::already_playing);
			return;
		}
		if (outcome.decision == enter_decision::kick_holder)
		{
			end_request(*session);
			const bool sent = m_worlds.send_kick(outcome.holder_world, account_id, hb::net::link::kick_reason::forced_login_elsewhere);
			log::info("account {}: force login, kick {} to world {}", account_id, sent ? "sent" : "not sent (link down)", outcome.holder_world);
			reply_enter(*session, proto::enter_result::force_disconnect_sent);
			return;
		}

		hb::net::link::session_grant grant{};
		grant.account_id = account_id;
		grant.character = request.character;
		if (!generate_ticket(grant.ticket))
		{
			online.release(account_id, outcome.generation);
			end_request(*session);
			reply_enter(*session, proto::enter_result::world_unavailable);
			return;
		}
		bool known = false;
		{
			std::scoped_lock lock(m_mutex);
			const auto found = m_sessions.find(session.get());
			known = found != m_sessions.end();
			if (known)
			{
				found->second.enter_generation = outcome.generation;
				found->second.ticket = grant.ticket;
			}
		}
		if (!known)
		{
			// The session closed since begin_enter: nobody can receive this ticket.
			online.release(account_id, outcome.generation);
			wipe(grant);
			return;
		}
		m_worlds.request_session_grant(world, grant, session->executor(),
			[this, weak = std::weak_ptr<hb::net::connection>(session), account_id, generation = outcome.generation](
				world_request_status status, hb::net::link::grant_result result) {
				if (const std::shared_ptr<hb::net::connection> alive = weak.lock())
					finish_enter(alive, account_id, generation, status, result);
				else
					m_worlds.online().release(account_id, generation);  // the client left before the answer
			});
		wipe(grant);
	}

	void authenticator::finish_enter(const std::shared_ptr<hb::net::connection>& session, std::uint64_t account_id, std::uint64_t generation,
		world_request_status status, hb::net::link::grant_result grant)
	{
		online_registry& online = m_worlds.online();
		hb::net::session_ticket ticket{};
		std::uint64_t unused_account = 0;
		std::string world;
		if (!take_enter_ticket(*session, ticket) || !session_target(*session, unused_account, world))
		{
			online.release(account_id, generation);
			wipe_ticket(ticket);
			return;
		}
		end_request(*session);
		if (!session->is_open())
		{
			online.release(account_id, generation);
			wipe_ticket(ticket);
			return;
		}

		proto::enter_result result = proto::enter_result::world_unavailable;
		if (status == world_request_status::answered)
		{
			switch (grant)
			{
			case hb::net::link::grant_result::ok:
				// Still the reservation this grant was made for: not expired, not revoked, the World still linked.
				if (online.is_pending(account_id, generation))
					result = proto::enter_result::ok;
				break;
			case hb::net::link::grant_result::world_full:
				result = proto::enter_result::world_full;
				break;
			case hb::net::link::grant_result::no_such_character:
				result = proto::enter_result::no_such_character;
				break;
			case hb::net::link::grant_result::shutting_down:
				result = proto::enter_result::world_shutting_down;
				break;
			}
		}
		if (result != proto::enter_result::ok)
		{
			online.release(account_id, generation);
			wipe_ticket(ticket);
			log::info("account {} refused entry to world {}: result {}", account_id, world, static_cast<unsigned>(result));
			reply_enter(*session, result);
			return;
		}

		const world_link_status where = m_worlds.status(world);
		proto::enter_game_response reply{};
		reply.result = proto::enter_result::ok;
		(void)reply.world_host.assign(where.client_host);
		reply.world_port = where.client_port;
		reply.ticket = ticket;
		wipe_ticket(ticket);
		(void)session->send(reply);
		wipe(reply);
		log::info("account {} handed off to world {}", account_id, world);
		// handed_off: the client now talks to the World, and this session is done.
		session->close_after_send(hb::net::close_reason::local_close, "handed off to the World");
	}
}
