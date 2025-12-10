#include "stdafx.h"
#include "UpdateChecker.h"
#include "SettingsProvider.h"
#include "resource.h"
#include <thread>
#include <sstream>
#include <algorithm>
#include <urlmon.h>
#include <wininet.h>
#include <cwctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wininet.lib")

// GitHub API URL - use const wchar_t* to avoid static initialization order issues
static const wchar_t* GITHUB_API_SERVER = L"api.github.com";
static const wchar_t* GITHUB_API_PATH = L"/repos/justnullname/QuickView/releases/latest";
static const wchar_t* USER_AGENT = L"QuickView-Updater/1.0";

void CUpdateChecker::CheckForUpdateAsync(HWND hNotifyWnd, UpdateCallback callback) {
	std::thread([hNotifyWnd, callback]() {
		CASyncUpdateResult result = CheckForUpdateSync();
		
		if (callback) {
			callback(result);
		}
	}).detach();
}

void CUpdateChecker::CheckAndDownloadAsync(HWND hNotifyWnd, UpdateCallback callback) {
	std::thread([hNotifyWnd, callback]() {
		CASyncUpdateResult result = CheckForUpdateSync();
		
		if (result.bSuccess) {
			CString sCurrentVer(JPEGVIEW_VERSION);
			std::wstring sCurrentVerW = (LPCTSTR)sCurrentVer;
			
			// Only download if newer
			if (CompareVersions(result.strLatestVersion, sCurrentVerW) > 0) {
				// Check if skipped
				CString sSkipped = CSettingsProvider::This().LastSkippedVersion();
				std::wstring sSkippedW = (LPCTSTR)sSkipped;
				
				if (sSkippedW != result.strLatestVersion) {
					// Prepare Update directory
					wchar_t thisPath[MAX_PATH];
					GetModuleFileName(NULL, thisPath, MAX_PATH);
					std::wstring exeDir = thisPath;
					size_t lastSlash = exeDir.find_last_of(L"\\/");
					if (lastSlash != std::wstring::npos) {
						exeDir = exeDir.substr(0, lastSlash);
					}
					
					std::wstring updateDir = exeDir + L"\\Update";
					CreateDirectory(updateDir.c_str(), NULL); // Create if not exists
					
					// Determine filename
					std::wstring strURL = result.strDownloadURL;
					std::wstring ext = L".exe";
					size_t lastDot = strURL.find_last_of(L'.');
					if (lastDot != std::wstring::npos) {
						std::wstring urlExt = strURL.substr(lastDot);
						if (urlExt.length() <= 5) ext = urlExt;
					}
					
					std::wstring targetPath = updateDir + L"\\update_package" + ext; // Fixed name to avoid accumulation
					
					// Clear cache
					DeleteUrlCacheEntry(strURL.c_str());
					
					HRESULT hr = URLDownloadToFile(NULL, strURL.c_str(), targetPath.c_str(), 0, NULL);
					if (SUCCEEDED(hr)) {
						result.strLocalPath = targetPath;
					}
				}
			}
		}

		if (callback) {
			callback(result);
		}
	}).detach();
}

CASyncUpdateResult CUpdateChecker::CheckForUpdateSync() {
	CASyncUpdateResult result = { false };
	
	std::wstring json = GetLatestReleaseJSON();
	if (json.empty()) {
		result.strErrorMessage = L"Failed to retrieve release info from GitHub.";
		return result;
	}

	result = ParseReleaseJSON(json);
	return result;
}

std::wstring CUpdateChecker::GetLatestReleaseJSON() {
	HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
	std::wstring response;

	hSession = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return L"";

	hConnect = WinHttpConnect(hSession, GITHUB_API_SERVER, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!hConnect) { WinHttpCloseHandle(hSession); return L""; }

	hRequest = WinHttpOpenRequest(hConnect, L"GET", GITHUB_API_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return L""; }

	if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hRequest, NULL)) {
		
		DWORD dwSize = 0;
		DWORD dwDownloaded = 0;

		do {
			dwSize = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
			if (dwSize == 0) break;

			char* tempBuffer = new char[dwSize + 1];
			ZeroMemory(tempBuffer, dwSize + 1);

			if (WinHttpReadData(hRequest, (LPVOID)tempBuffer, dwSize, &dwDownloaded)) {
				int nLen = MultiByteToWideChar(CP_UTF8, 0, tempBuffer, dwDownloaded, NULL, 0);
				if (nLen > 0) {
					std::wstring wFrag(nLen, 0);
					MultiByteToWideChar(CP_UTF8, 0, tempBuffer, dwDownloaded, &wFrag[0], nLen);
					response.append(wFrag);
				}
			}
			delete[] tempBuffer;
		} while (dwSize > 0);
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	return response;
}

// Simple JSON parser for specific fields
CASyncUpdateResult CUpdateChecker::ParseReleaseJSON(const std::wstring& json) {
	CASyncUpdateResult result;
	result.bSuccess = false;

	// Helper to find value by key
	auto FindValue = [&](const std::wstring& key) -> std::wstring {
		size_t keyPos = json.find(L"\"" + key + L"\"");
		if (keyPos == std::wstring::npos) return L"";
		
		size_t valStart = json.find(L":", keyPos);
		if (valStart == std::wstring::npos) return L"";
		
		// Find start of string value
		size_t openQuote = json.find(L"\"", valStart);
		if (openQuote == std::wstring::npos) return L"";
		
		size_t closeQuote = json.find(L"\"", openQuote + 1);
		while (closeQuote != std::wstring::npos && json[closeQuote - 1] == L'\\') {
			// Skip escaped quotes
			closeQuote = json.find(L"\"", closeQuote + 1);
		}
		if (closeQuote == std::wstring::npos) return L"";

		return json.substr(openQuote + 1, closeQuote - openQuote - 1);
	};

	result.strLatestVersion = FindValue(L"tag_name");
	result.strReleaseNotes = FindValue(L"body");
	
	// Find download URL (search for first asset ending in .exe)
	size_t assetsPos = json.find(L"\"assets\"");
	if (assetsPos != std::wstring::npos) {
		size_t searchPos = assetsPos;
		while (true) {
			size_t urlKey = json.find(L"\"browser_download_url\"", searchPos);
			if (urlKey == std::wstring::npos) break;

			size_t valStart = json.find(L":", urlKey);
			size_t openQuote = json.find(L"\"", valStart);
			size_t closeQuote = json.find(L"\"", openQuote + 1);
			if (closeQuote == std::wstring::npos) break;

			std::wstring url = json.substr(openQuote + 1, closeQuote - openQuote - 1);
			
			// Check file extension
			if (url.length() > 4) {
				std::wstring ext = url.substr(url.length() - 4);
				std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
				
				if (ext == L".exe") {
					result.strDownloadURL = url;
					break; // Prefer .exe
				} else if (ext == L".zip" && result.strDownloadURL.empty()) {
					result.strDownloadURL = url;
					// Keep searching in case there is an .exe later
				}
			}
			searchPos = closeQuote;
		}
	}

	if (!result.strLatestVersion.empty() && !result.strDownloadURL.empty()) {
		result.bSuccess = true;
	}

	return result;
}

std::wstring CUpdateChecker::DownloadUpdateSync(const std::wstring& strURL) {
	wchar_t tempPath[MAX_PATH];
	GetTempPath(MAX_PATH, tempPath);
	
	wchar_t tempFile[MAX_PATH];
	GetTempFileName(tempPath, L"QVU", 0, tempFile);
	
	std::wstring strTarget = tempFile;
	
	// Determine extension from URL
	std::wstring ext = L".exe"; // Default
	size_t lastDot = strURL.find_last_of(L'.');
	if (lastDot != std::wstring::npos) {
		std::wstring urlExt = strURL.substr(lastDot);
		if (urlExt.length() <= 5) { // Sanity check
			ext = urlExt;
		}
	}
	
	strTarget += ext;
	MoveFile(tempFile, strTarget.c_str());

	// Clear cache to ensure fresh download
	DeleteUrlCacheEntry(strURL.c_str());

	HRESULT hr = URLDownloadToFile(NULL, strURL.c_str(), strTarget.c_str(), 0, NULL);
	if (SUCCEEDED(hr)) {
		return strTarget;
	}
	
	DeleteFile(strTarget.c_str());
	return L"";
}

bool CUpdateChecker::InstallUpdate(const std::wstring& localFile) {
	if (localFile.empty()) return false;

	TCHAR thisPath[MAX_PATH];
	GetModuleFileName(NULL, thisPath, MAX_PATH);
	
	TCHAR tempPath[MAX_PATH];
	GetTempPath(MAX_PATH, tempPath);

	CString batPath;
	batPath.Format(_T("%supdate_qv.bat"), tempPath);
	
	FILE* f = NULL;
	_tfopen_s(&f, batPath, _T("w"));
	if (f) {
		bool bIsZip = false;
		if (localFile.length() > 4) {
			std::wstring ext = localFile.substr(localFile.length() - 4);
			std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c){ return std::towlower(c); });
			if (ext == L".zip") bIsZip = true;
		}

		_ftprintf(f, _T("@echo off\n"));
		_ftprintf(f, _T("timeout /t 2 /nobreak > nul\n"));
		
		if (bIsZip) {
			// ZIP handling
			_ftprintf(f, _T("mkdir \"%s_extract\"\n"), tempPath);
			// PowerShell unzipping
			_ftprintf(f, _T("powershell -Command \"Expand-Archive -Path '%s' -DestinationPath '%s_extract' -Force\"\n"), localFile.c_str(), tempPath);
			
			// Copy QuickView.exe from extracted folder (recursive search)
			_ftprintf(f, _T("for /r \"%s_extract\" %%%%f in (QuickView.exe) do copy /Y \"%%%%f\" \"%s\"\n"), tempPath, thisPath);
			
			// Clean up
			_ftprintf(f, _T("rmdir /s /q \"%s_extract\"\n"), tempPath);
		} else {
			// EXE handling
			_ftprintf(f, _T("copy /Y \"%s\" \"%s\"\n"), localFile.c_str(), thisPath);
		}
		
		_ftprintf(f, _T("start \"\" \"%s\"\n"), thisPath);
		_ftprintf(f, _T("del \"%%~f0\"\n"));
		fclose(f);
		
		// Execute batch
		ShellExecute(NULL, _T("open"), batPath, NULL, NULL, SW_HIDE);
		return true;
	}
	return false;
}

int CUpdateChecker::CompareVersions(const std::wstring& v1, const std::wstring& v2) {

	// Simple version compare (e.g. "1.1.5" vs "1.1.6")
	// Returns 1 if v1 > v2, -1 if v1 < v2, 0 if equal
	
	// Strip "v" prefix if present
	std::wstring s1 = v1;
	std::wstring s2 = v2;
	if (!s1.empty() && (s1[0] == L'v' || s1[0] == L'V')) s1 = s1.substr(1);
	if (!s2.empty() && (s2[0] == L'v' || s2[0] == L'V')) s2 = s2.substr(1);

	int parts1[4] = { 0 }, parts2[4] = { 0 };
	swscanf_s(s1.c_str(), L"%d.%d.%d.%d", &parts1[0], &parts1[1], &parts1[2], &parts1[3]);
	swscanf_s(s2.c_str(), L"%d.%d.%d.%d", &parts2[0], &parts2[1], &parts2[2], &parts2[3]);

	for (int i = 0; i < 4; i++) {
		if (parts1[i] > parts2[i]) return 1;
		if (parts1[i] < parts2[i]) return -1;
	}
	return 0;
}
