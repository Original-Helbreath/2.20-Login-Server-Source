#include "authenticator.h"

#include "asio_config.h"
#include "log.h"
#include "login_messages.h"
#include "message_codec.h"
#include "message_id.h"

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
		return options;
	}

	authenticator::authenticator(account_service& accounts, worker_pool& workers, authenticator_options options)
		: m_accounts(accounts)
		, m_workers(workers)
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
		else
			log::debug("client {}: message 0x{:04x} not handled yet (LOGIN-007)", from.remote_address(), id);
	}

	void authenticator::on_closed(const hb::net::connection& session)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_sessions.find(&session);
		if (found == m_sessions.end())
			return;
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
		{
			const std::shared_ptr<hb::net::connection> self = session.shared_from_this();
			std::shared_ptr<hb::net::connection> previous;
			{
				std::scoped_lock lock(m_mutex);
				session_state& state = m_sessions[&session];
				state.authenticated = true;
				state.account_id = outcome.account_id;
				const auto live = m_live.find(outcome.account_id);
				if (live != m_live.end())
				{
					previous = live->second.lock();
					live->second = self;
				}
				else
				{
					m_live.emplace(outcome.account_id, self);
				}
			}
			if (previous && previous.get() != &session)
			{
				log::info("account {} logged in again from {}; closing the session from {}", outcome.name, address, previous->remote_address());
				previous->close(hb::net::close_reason::local_close, "superseded by a second login");
			}
			else
			{
				log::info("account {} logged in from {}", outcome.name, address);
			}
			// Logged in: the pre-login deadline no longer applies and the longer idle timeout takes over.
			session.set_deadline(std::chrono::milliseconds(0));
			session.set_idle_timeout(m_options.idle_after_login);
			reply.result = proto::login_result::ok;
			break;
		}
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
}
