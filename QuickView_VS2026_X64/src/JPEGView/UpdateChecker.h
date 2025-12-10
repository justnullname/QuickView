#pragma once

#include <string>
#include <functional>
#include <Windows.h>
#include <winhttp.h>

// Result structure for async update check
struct CASyncUpdateResult {
	bool bSuccess;
	std::wstring strLatestVersion;
	std::wstring strReleaseNotes;
	std::wstring strDownloadURL;
	std::wstring strLocalPath; // Path to downloaded file if successful
	std::wstring strErrorMessage;
};

// Callback type for async update check
typedef std::function<void(const CASyncUpdateResult&)> UpdateCallback;

class CUpdateChecker {
public:
	// Check for updates asynchronously, calls callback when done
	static void CheckForUpdateAsync(HWND hNotifyWnd, UpdateCallback callback);

	// Check for updates AND download if available, calls callback when done
	static void CheckAndDownloadAsync(HWND hNotifyWnd, UpdateCallback callback);
	
	// Check for updates synchronously
	
	// Check for updates synchronously
	static CASyncUpdateResult CheckForUpdateSync();
	
	// Download update file to temp, returns path or empty on failure
	static std::wstring DownloadUpdateSync(const std::wstring& strURL);
	
	// Create and execute update script (handles .exe and .zip)
	static bool InstallUpdate(const std::wstring& localFile);

	// Compare version strings (e.g. "v1.1.5" vs "v1.1.6")
	// Returns: 1 if v1 > v2, -1 if v1 < v2, 0 if equal
	static int CompareVersions(const std::wstring& v1, const std::wstring& v2);

private:
	static std::wstring GetLatestReleaseJSON();
	static CASyncUpdateResult ParseReleaseJSON(const std::wstring& json);
};
