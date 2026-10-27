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
	bool create_private_file(const std::filesystem::path& path, std::string& error)
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

		// Protected: the folder's inherited entries are not added, so only the owner has access from creation on.
		SECURITY_DESCRIPTOR descriptor{};
		if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
			|| !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
			|| !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
		{
			error = std::format("cannot build a security descriptor (error {})", GetLastError());
			return false;
		}
		SECURITY_ATTRIBUTES attributes{ sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE };
		HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = std::format("cannot create {} (error {})", path.string(), GetLastError());
			return false;
		}
		CloseHandle(file);
		return true;
	}
}

#endif
