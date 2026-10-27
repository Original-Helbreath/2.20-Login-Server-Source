#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "private_file.h"

#include <format>
#include <memory>
#include <vector>

#include <windows.h>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")

namespace hb::server::platform
{
	bool restrict_to_owner(const std::filesystem::path& path, std::string& error)
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		{
			error = std::format("cannot open the process token (error {})", GetLastError());
			return false;
		}
		const std::unique_ptr<void, decltype(&CloseHandle)> token_owner(token, CloseHandle);
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<std::byte> buffer(size);
		if (size == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
		{
			error = std::format("cannot read the current user (error {})", GetLastError());
			return false;
		}
		const TOKEN_USER* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());

		EXPLICIT_ACCESSW access{};
		access.grfAccessPermissions = GENERIC_ALL;
		access.grfAccessMode = SET_ACCESS;
		access.grfInheritance = NO_INHERITANCE;
		access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		access.Trustee.TrusteeType = TRUSTEE_IS_USER;
		access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);
		PACL acl = nullptr;
		const DWORD acl_result = SetEntriesInAclW(1, &access, nullptr, &acl);
		if (acl_result != ERROR_SUCCESS)
		{
			error = std::format("cannot build an owner-only access list (error {})", acl_result);
			return false;
		}
		const std::unique_ptr<ACL, decltype(&LocalFree)> acl_owner(acl, LocalFree);
		// PROTECTED drops the inherited entries, so only the owner keeps access.
		const DWORD set_result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
		if (set_result != ERROR_SUCCESS)
		{
			error = std::format("cannot restrict {} (error {})", path.string(), set_result);
			return false;
		}
		return true;
	}
}

#endif
