
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <lmcons.h>

#include "procfilter/procfilter.h"

#include <cctype>
#include <array>
#include <map>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <set>
#include <regex>
#include <vector>

using std::wregex;
using std::string;
using std::wstring;
using std::ifstream;
using std::set;

typedef std::basic_string<BYTE> Hash;

class RegexData {
public:
	RegexData(const wstring &regexString) :
		regexString(regexString),
		regexObjectInsensitive(regexString, wregex::icase),
		regexObjectSensitive(regexString)
	{
	}

	//
	// Determinte if the given string matches the object's associated regex
	//
	bool matchesString(const wstring &str, bool bCaseSensitive) const {
		if (bCaseSensitive) {
			return std::regex_search(str, regexObjectSensitive);
		} else {
			return std::regex_search(str, regexObjectInsensitive);
		}
	}

	const wstring regexString;
	const wregex regexObjectInsensitive;
	const wregex regexObjectSensitive;
};
typedef std::vector<RegexData> RegexVector;

class StringContainer {
public:
	StringContainer() {}

	bool contains(const std::wstring &str, bool bCaseSensitive) const {
		return bCaseSensitive ? case_strings.find(str) != case_strings.end() : icase_strings.find(str) != icase_strings.end();
	}

	void insert(const std::wstring &str) {
		case_strings.insert(str);

		std::wstring lstr = str;
		auto lowercase_wchar = [](wchar_t c)->wchar_t{
			if (iswascii(c) && iswupper(c)) {
				return static_cast<wchar_t>(towlower(c));
			} else {
				return c;
			}
		};
		std::transform(lstr.begin(), lstr.end(), lstr.begin(), lowercase_wchar);
		icase_strings.insert(lstr);
	}

private:
	bool (*icmp)(const std::wstring &lhs, const std::wstring &rhs){[](const std::wstring &lhs, const std::wstring &rhs) { return _wcsicmp(lhs.c_str(), rhs.c_str()) < 0; }};
	std::set<std::wstring, decltype(icmp)> icase_strings{icmp};
	std::set<std::wstring> case_strings;
};


// Skip whitespace
const char* skip_whitespace(const char *p) {
	while (*p != 0 && isspace(*p)) ++p;
	return p;
}

// Skip trailing whitespace
size_t strlen_no_trailing_whitespace(const char *p) {
	size_t len = strlen(p);
	while (len > 0 && isspace(p[len - 1])) {
		len -= 1;
	}
	return len;
}


std::wstring path_basename(const std::wstring &str) {
	const WCHAR *bs = wcsrchr(str.c_str(), L'\\');
	const WCHAR *fs = wcsrchr(str.c_str(), L'/');

	const WCHAR *p = nullptr;
	if (bs && fs) {
		p = bs > fs ? bs : fs;
	} else if (bs) {
		p = bs;
	} else if (fs) {
		p = fs;
	} else {
		return str;
	}

	return &p[1];
}


class List {
public:
	List() { }

	bool loadHashfile(PROCFILTER_EVENT *e, size_t &nhashes, const WCHAR *lpszFileName) {
		bool rv = false;
		nhashes = 0;

		ifstream infile(lpszFileName);
		if (infile.fail()) {
			return rv;
		}

		size_t linenum = 0;
		string line;
		while (std::getline(infile, line)) {
			++linenum;

			// Ignore comment lines
			if (line.length() == 0) continue;

			// File regexes
			std::wstring value;
			if (getTaggedValue(line, "filenameregex:", value)) {
				loadFilename(e, value, true);
			} else if (getTaggedValue(line, "filename:", value)) {
				loadFilename(e, value, false);
			} else if (getTaggedValue(line, "filebasenameregex:", value)) {
				loadFileBasename(e, value, true);
			} else if (getTaggedValue(line, "filebasename:", value)) {
				loadFileBasename(e, value, false);
			} else if (getTaggedValue(line, "usernameregex:", value)) {
				loadUsername(e, value, true);
			} else if (getTaggedValue(line, "username:", value)) {
				loadUsername(e, value, false);
			} else if (getTaggedValue(line, "groupnameregex:", value)) {
				loadGroupname(e, value, true);
			} else if (getTaggedValue(line, "groupname:", value)) {
				loadGroupname(e, value, false);
			} else {
				if (line[0] == '#' || line[0] == ';') continue;

				// Erase commentted portion of lines
				auto comment = line.find_first_of('#');
				if (comment != string::npos) line.erase(comment);
				comment = line.find_first_of(';');
				if (comment != string::npos) line.erase(comment);

				// Clear whitespace
				auto space_begin = std::remove_if(line.begin(), line.end(), [](char c) { return std::isspace(c); });
				line.erase(space_begin, line.end());

				if (line.length() == 0) continue;

				// MD5, SHA1, SHA256
				Hash baRawDigest;
				bool bHashValidLength = line.length() == 32 || line.length() == 40 || line.length() == 64;
				bool bParseSuccess = true;
				if (bHashValidLength) {
					size_t digest_length = line.length() / 2;

					baRawDigest.reserve(digest_length);
					for (size_t i = 0; i < digest_length; ++i) {
						int value = 0;
						if (sscanf(&line.c_str()[i * 2], "%2x", &value) == 1) {
							baRawDigest.push_back(value & 0xFF);
						} else {
							bParseSuccess = false;
							break;
						}
					}
				}

				if (bHashValidLength && bParseSuccess) {
					hashes.insert(baRawDigest);
					nhashes += 1;
				} else {
					e->LogFmt("Invalid hash in %ls on line %zu", lpszFileName, linenum);
				}
			}
		}

		return true;
	}

	void loadFilename(PROCFILTER_EVENT *e, const std::wstring &str, bool bRegex) {
		// Try to conver the value to a DOS path
		WCHAR szNtPath[4096];
		WCHAR szDosDevice[MAX_PATH + 1];
		std::wstring result = str;
		if (e->GetNtPathName(str.c_str(), szDosDevice, sizeof(szDosDevice), szNtPath, sizeof(szNtPath), NULL, 0)) {
			wstring wsDosDevice{ szDosDevice };
			size_t pos = 0;
			if (bRegex) {
				while ((pos = wsDosDevice.find(L"\\", pos)) != wstring::npos) {
					wsDosDevice.replace(pos, 1, L"\\\\");
					pos += 2;
				}
				result = wstring{ LR"(\\\\\?\\GLOBALROOT)" } + wsDosDevice + szNtPath;
			} else {
				result = wstring{ LR"(\\?\GLOBALROOT)" } + wsDosDevice + szNtPath;
			}
		}

		// Add the regex to the container
		if (bRegex) {
			try {
				filenameRegexes.push_back(RegexData(result));
			} catch (std::regex_error &error) {
				e->LogFmt("Regex compilation failure for value: %ls\nError: %s", str.c_str(), error.what());
			}
		} else {
			filenames.insert(result);
		}
	}
	
	void loadFileBasename(PROCFILTER_EVENT *e, const std::wstring &value, bool bRegex) {
		if (bRegex) {
			tryAddRegex(e, filebasenameRegexes, value);
		} else {
			filebasenames.insert(value);
		}
	}

	void loadUsername(PROCFILTER_EVENT *e, const std::wstring &value, bool bRegex) {
		if (bRegex) {
			tryAddRegex(e, usernameRegexes, value);
		} else {
			usernames.insert(value);
		}
	}

	void loadGroupname(PROCFILTER_EVENT *e, const std::wstring &value, bool bRegex) {
		if (bRegex) {
			tryAddRegex(e, groupnameRegexes, value);
		} else {
			groupnames.insert(value);
		}
	}

	void loadHashFileFromBasename(PROCFILTER_EVENT *e, const WCHAR *szBasename) {
		WCHAR szFullPath[MAX_PATH + 1];

		e->GetProcFilterPath(szFullPath, sizeof(szFullPath), L"localrules", szBasename);
		size_t nhashes = 0;
		if (loadHashfile(e, nhashes, szFullPath)) e->LogFmt("Loaded %zu hashes from %ls", nhashes, szFullPath);

		e->GetProcFilterPath(szFullPath, sizeof(szFullPath), L"remoterules", szBasename);
		nhashes = 0;
		if (loadHashfile(e, nhashes, szFullPath)) e->LogFmt("Loaded %zu hashes from %ls", nhashes, szFullPath);
	}

	bool containsAnyHash(const HASHES *hashes) const {
		bool bResult = containsHash(hashes->md5_digest, MD5_DIGEST_SIZE);
		if (!bResult) bResult = containsHash(hashes->sha1_digest, SHA1_DIGEST_SIZE);
		if (!bResult) bResult = containsHash(hashes->sha256_digest, SHA256_DIGEST_SIZE);
		return bResult;
	}

	bool containsHash(const BYTE *hash, size_t hash_size) const {
		Hash value{ (BYTE*)hash, hash_size };
		return hashes.find(value) != hashes.end();
	}

	bool matchesFilename(const std::wstring &str) const {
		std::wstring basenamestr = path_basename(str);
		return strMatch(filenames, filenameRegexes, str, false) || strMatch(filebasenames, filebasenameRegexes, basenamestr, false);
	}

	bool matchesUsername(const std::wstring &str) const {
		return strMatch(usernames, usernameRegexes, str, false);
	}

	bool matchesGroupname(const std::wstring &str) const {
		return strMatch(groupnames, groupnameRegexes, str, false);
	}

private:
	bool getTaggedValue(const std::string &line, const char *tag, std::wstring &result) {
		result.clear();
		size_t taglen = strlen(tag);
		if (_strnicmp(line.c_str(), tag, taglen) == 0) {
			const char *p = &line[taglen];
			p = skip_whitespace(p);
			size_t len = strlen_no_trailing_whitespace(p);
			if (len > 0) {
				std::string value = std::string{p, len};
				result.reserve(value.size());
				result.assign(value.begin(), value.end());
				return true;
			}
		}

		return false;
	}

	void tryAddRegex(PROCFILTER_EVENT *e, RegexVector &rev, const std::wstring &re) {
		try {
			rev.push_back(RegexData(re));
		} catch (std::regex_error &error) {
			e->LogFmt("Regex compilation failure for value: %ls\nError: %s", re.c_str(), error.what());
		}
	}

	static bool strMatch(const StringContainer &sc, const RegexVector &rv, const std::wstring &lpszString, bool bCaseSensitive) {
		if (sc.contains(lpszString, bCaseSensitive)) return true;

		for (const auto &re : rv) {
			if (re.matchesString(lpszString, bCaseSensitive)) {
				return true;
			}
		}

		return false;
	}

	set<Hash> hashes;
	StringContainer filenames;
	RegexVector filenameRegexes;
	StringContainer filebasenames;
	RegexVector filebasenameRegexes;
	StringContainer usernames;
	StringContainer groupnames;
	RegexVector usernameRegexes;
	RegexVector groupnameRegexes;
};

static List g_Whitelist;
static List g_WhitelistExceptions;
static List g_Blacklist;

static CRITICAL_SECTION g_cs;
static set<DWORD> g_WhitelistedPids;

static bool g_HashExes = true;
static bool g_LogRemoteThreads = false;
static bool g_HashDlls = false;
static bool g_LogLoadedDllNames = false;
static bool g_LogCommandLine = true;
static bool g_AlwaysLog = true;
static DWORD g_MaxHashFileSize = 3 * 1024 * 1024;
static DWORD g_MaxQuarantineFileSize = 3 * 1024 * 1024;

static WCHAR g_szCommandLineRuleFileBaseName[MAX_PATH + 1] = { '\0' };
static __declspec(thread) YARASCAN_CONTEXT *tg_CommandLineRulesContext = NULL;

static WCHAR g_szFilenameRuleFileBaseName[MAX_PATH + 1] = { '\0' };
static __declspec(thread) YARASCAN_CONTEXT *tg_FilenameRulesContext = NULL;

static WCHAR g_szParentFilenameRuleFileBaseName[MAX_PATH + 1] = { '\0' };
static __declspec(thread) YARASCAN_CONTEXT *tg_ParentFilenameRulesContext = NULL;


static
void
LoadRulesFile(PROCFILTER_EVENT *e, const WCHAR *lpszRuleFilename, YARASCAN_CONTEXT **o_RulesContext)
{
	YARASCAN_CONTEXT *result = NULL;

	if (wcslen(lpszRuleFilename) > 0) {
		WCHAR szError[256] = { '\0' };
		result = e->AllocateScanContextLocalAndRemote(lpszRuleFilename, szError, sizeof(szError), true);
		if (!result) {
			e->LogFmt("Error compiling rules file %ls: %ls", lpszRuleFilename, szError);
		}
	}

	*o_RulesContext = result;
}

static
bool
GetUserNameAndGroupFromToken(HANDLE hToken, WCHAR *lpszName, DWORD dwNameSize, WCHAR *lpszGroup, DWORD dwGroupSize)
{
	// TokenOwner for group name
	BYTE buf[512] = { '\0' };
	TOKEN_USER *lpTokenUser = (TOKEN_USER*)buf;
	DWORD dwTokenUserSize = sizeof(buf);

	DWORD dwResultSize = 0;
	BOOL rc = GetTokenInformation(hToken, TokenUser, lpTokenUser, dwTokenUserSize, &dwResultSize);
	if (!rc && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
		lpTokenUser = (TOKEN_USER*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwResultSize);
		if (lpTokenUser) {
			rc = GetTokenInformation(hToken, TokenUser, lpTokenUser, dwResultSize, &dwResultSize);
		}
	}

	if (rc) {
		dwNameSize /= sizeof(WCHAR);
		dwGroupSize /= sizeof(WCHAR);
		SID_NAME_USE SidType;
		rc = LookupAccountSidW(NULL, lpTokenUser->User.Sid, lpszName, &dwNameSize, lpszGroup, &dwGroupSize, &SidType);
	}

	if (lpTokenUser && (void*)lpTokenUser != (void*)buf) {
		HeapFree(GetProcessHeap(), 0, lpTokenUser);
	}

	return rc ? true : false;
}

static
bool
GetUsernameAndGroupFromPid(DWORD dwProcessId, std::wstring &username, std::wstring &groupname)
{
	bool rv = false;

	username = L"";
	groupname = L"";

	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, dwProcessId);
	if (hProcess != NULL) {
		HANDLE hToken = NULL;
		if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
			WCHAR szUserName[UNLEN+1+1] = { '\0' };
			WCHAR szGroupName[GNLEN+1+1] = { '\0' };
			if (GetUserNameAndGroupFromToken(hToken, szUserName, sizeof(szUserName)-sizeof(WCHAR), szGroupName, sizeof(szGroupName)-sizeof(WCHAR))) {
				username = szUserName;
				groupname = szGroupName;
				rv = true;
			}
			CloseHandle(hToken);
		}
		CloseHandle(hProcess);
	}

	return rv;
}


void
AsciiAndUnicodeScan(PROCFILTER_EVENT *e, YARASCAN_CONTEXT *ctx, const WCHAR *lpszTarget, SCAN_RESULT *srUnicodeResult, SCAN_RESULT *srAsciiResult)
{
	if (!lpszTarget || *lpszTarget == 0) return;

	const DWORD dwTargetCharCount = (DWORD)wcslen(lpszTarget);

	// Scan the UNICODE command line and log the result
	e->ScanData(ctx, lpszTarget, dwTargetCharCount * sizeof(WCHAR), NULL, NULL, NULL, srUnicodeResult);

	// Scan with ASCII (use the same number of bytes though)
	DWORD dwAsciiByteCount = dwTargetCharCount*sizeof(WCHAR) + sizeof(WCHAR);
	char *lpszAsciiCommandLine = (char*)e->AllocateMemory(dwAsciiByteCount, sizeof(char));
	snprintf(lpszAsciiCommandLine, dwAsciiByteCount-1, "%ls", lpszTarget);

	// Scan the ASCII command line
	e->ScanData(ctx, lpszAsciiCommandLine, (DWORD)strlen(lpszAsciiCommandLine), NULL, NULL, NULL, srAsciiResult);

	// Cleanup
	e->FreeMemory(lpszAsciiCommandLine);
}


DWORD
ProcFilterEvent(PROCFILTER_EVENT *e)
{
	DWORD dwResultFlags = PROCFILTER_RESULT_NONE;

	if (e->dwEventId == PROCFILTER_EVENT_INIT) {
		e->RegisterPlugin(PROCFILTER_VERSION, L"Core", 0, 0, false, PROCFILTER_EVENT_PROCESS_CREATE, PROCFILTER_EVENT_NONE);

		InitializeCriticalSection(&g_cs);
		
		e->GetConfigString(L"CommandLineRules", L"commandline.yara", g_szCommandLineRuleFileBaseName, sizeof(g_szCommandLineRuleFileBaseName));
		e->GetConfigString(L"FilenameRules", L"filename.yara", g_szFilenameRuleFileBaseName, sizeof(g_szFilenameRuleFileBaseName));
		e->GetConfigString(L"ParentFilenameRules", L"parentfilename.yara", g_szParentFilenameRuleFileBaseName, sizeof(g_szParentFilenameRuleFileBaseName));
		
		g_AlwaysLog = e->GetConfigBool(L"AlwaysLog", g_AlwaysLog);

		g_HashDlls = e->GetConfigBool(L"HashDlls", g_HashDlls);
		g_LogLoadedDllNames = e->GetConfigBool(L"LogLoadedDllNames", g_LogLoadedDllNames);
		if (g_HashDlls || g_LogLoadedDllNames) e->EnableEvent(PROCFILTER_EVENT_IMAGE_LOAD);

		g_LogRemoteThreads = e->GetConfigBool(L"LogRemoteThreads", g_LogRemoteThreads);
		if (g_LogRemoteThreads) e->EnableEvent(PROCFILTER_EVENT_THREAD_CREATE);

		WCHAR szListBasename[MAX_PATH + 1];
		e->GetConfigString(L"WhitelistFilename", L"whitelist.txt", szListBasename, sizeof(szListBasename));
		if (szListBasename[0]) g_Whitelist.loadHashFileFromBasename(e, szListBasename);

		e->GetConfigString(L"WhitelistExceptionsFilename", L"whitelist_exceptions.txt", szListBasename, sizeof(szListBasename));
		if (szListBasename[0]) g_WhitelistExceptions.loadHashFileFromBasename(e, szListBasename);

		e->GetConfigString(L"BlacklistFilename", L"blacklist.txt", szListBasename, sizeof(szListBasename));
		if (szListBasename[0]) g_Blacklist.loadHashFileFromBasename(e, szListBasename);
		
		g_LogCommandLine = e->GetConfigBool(L"LogCommandLineArguments", g_LogCommandLine);

		g_MaxHashFileSize = (DWORD)e->GetConfigInt(L"MaxHashFileSize", (int)g_MaxHashFileSize);
		g_MaxQuarantineFileSize = (DWORD)e->GetConfigInt(L"MaxQuarantineFileSize", (int)g_MaxQuarantineFileSize);

		g_HashExes = e->GetConfigBool(L"HashExes", g_HashExes);
	} else if (e->dwEventId == PROCFILTER_EVENT_SHUTDOWN) {
		DeleteCriticalSection(&g_cs);
	} else if (e->dwEventId == PROCFILTER_EVENT_PROCFILTER_THREAD_INIT) {
		LoadRulesFile(e, g_szCommandLineRuleFileBaseName, &tg_CommandLineRulesContext);
		LoadRulesFile(e, g_szFilenameRuleFileBaseName, &tg_FilenameRulesContext);
		LoadRulesFile(e, g_szParentFilenameRuleFileBaseName, &tg_ParentFilenameRulesContext);
	} else if (e->dwEventId == PROCFILTER_EVENT_PROCFILTER_THREAD_SHUTDOWN) {
		if (tg_CommandLineRulesContext) e->FreeScanContext(tg_CommandLineRulesContext);
		if (tg_FilenameRulesContext) e->FreeScanContext(tg_FilenameRulesContext);
		if (tg_ParentFilenameRulesContext) e->FreeScanContext(tg_ParentFilenameRulesContext);
	} else if (e->dwEventId == PROCFILTER_EVENT_PROCESS_CREATE && e->lpszFileName) {
		// Ignore whitelisted files
		if (!g_WhitelistExceptions.matchesFilename(e->lpszFileName) && g_Whitelist.matchesFilename(e->lpszFileName)) {
			EnterCriticalSection(&g_cs);
			g_WhitelistedPids.insert(e->dwProcessId);
			LeaveCriticalSection(&g_cs);
			return PROCFILTER_RESULT_DONT_SCAN;
		}

		// Check user/group whitelists
		std::wstring username;
		std::wstring groupname;
		bool bHaveUserAndGroup = GetUsernameAndGroupFromPid(e->dwProcessId, username, groupname);
		if (bHaveUserAndGroup && !g_WhitelistExceptions.matchesUsername(username) && !g_WhitelistExceptions.matchesGroupname(groupname)) {
			if (g_Whitelist.matchesUsername(username) || g_Whitelist.matchesGroupname(groupname)) {
				EnterCriticalSection(&g_cs);
				g_WhitelistedPids.insert(e->dwProcessId);
				LeaveCriticalSection(&g_cs);
				return PROCFILTER_RESULT_DONT_SCAN;
			}
		}

		// Filename blacklisted?
		bool bFilenameBlacklisted = g_Blacklist.matchesFilename(e->lpszFileName);

		HASHES hashes;
		ZeroMemory(&hashes, sizeof(HASHES));

		bool bHashBlacklisted = false;
		bool bHashSuccessful = false;
		if (g_HashExes) {
			bHashSuccessful = e->HashFile(e->lpszFileName, g_MaxHashFileSize, &hashes);

			if (bHashSuccessful) {
				if (!g_WhitelistExceptions.containsAnyHash(&hashes)) {
					if (g_Whitelist.containsAnyHash(&hashes)) {
						EnterCriticalSection(&g_cs);
						g_WhitelistedPids.insert(e->dwProcessId);
						LeaveCriticalSection(&g_cs);
						return PROCFILTER_RESULT_DONT_SCAN;
					}
				}

				// Check if the hash is blocked
				bHashBlacklisted = g_Blacklist.containsAnyHash(&hashes);
			}
		}

		// Username blacklisted?
		bool bUsernameBlacklisted = false;
		bool bGroupnameBlacklisted = false;
		if (bHaveUserAndGroup) {
			bUsernameBlacklisted = g_Blacklist.matchesUsername(username);
			bGroupnameBlacklisted = g_Blacklist.matchesUsername(groupname);
		}

		//
		// Scan command lines (Two since there's UNICODE & ASCII)
		//
		SCAN_RESULT srCommandLineAsciiResult;
		SCAN_RESULT srCommandLineUnicodeResult;
		ZeroMemory(&srCommandLineAsciiResult, sizeof(SCAN_RESULT));
		ZeroMemory(&srCommandLineUnicodeResult, sizeof(SCAN_RESULT));
		const WCHAR *lpszCommandLine = e->GetProcessCommandLine();
		if (lpszCommandLine && tg_CommandLineRulesContext) {
			AsciiAndUnicodeScan(e, tg_CommandLineRulesContext, lpszCommandLine, &srCommandLineUnicodeResult, &srCommandLineAsciiResult);
		} else {
			lpszCommandLine = L"";
		}

		//
		// Scan filenames (Two since there's UNICODE & ASCII)
		//
		SCAN_RESULT srFilenameAsciiResult;
		SCAN_RESULT srFilenameUnicodeResult;
		ZeroMemory(&srFilenameAsciiResult, sizeof(SCAN_RESULT));
		ZeroMemory(&srFilenameUnicodeResult, sizeof(SCAN_RESULT));
		if (e->lpszFileName && tg_FilenameRulesContext) {
			AsciiAndUnicodeScan(e, tg_FilenameRulesContext, e->lpszFileName, &srFilenameUnicodeResult, &srFilenameAsciiResult);
		}

		// Get parent name
		WCHAR szParentName[MAX_PATH + 1] = { 0 };
		if (!e->GetProcessFileName(e->dwParentProcessId, szParentName, sizeof(szParentName))) {
			szParentName[0] = 0;
		}

		SCAN_RESULT srParentFilenameAsciiResult;
		SCAN_RESULT srParentFilenameUnicodeResult;
		ZeroMemory(&srParentFilenameAsciiResult, sizeof(SCAN_RESULT));
		ZeroMemory(&srParentFilenameUnicodeResult, sizeof(SCAN_RESULT));
		if (szParentName[0] && tg_ParentFilenameRulesContext) {
			AsciiAndUnicodeScan(e, tg_ParentFilenameRulesContext, szParentName, &srParentFilenameUnicodeResult, &srParentFilenameAsciiResult);
		}

		bool bBlockProcess = (bHashBlacklisted || bFilenameBlacklisted || bUsernameBlacklisted || bGroupnameBlacklisted ||
				(srCommandLineUnicodeResult.bScanSuccessful && srCommandLineUnicodeResult.bBlock) ||
				(srCommandLineAsciiResult.bScanSuccessful && srCommandLineAsciiResult.bBlock) ||
				(srFilenameUnicodeResult.bScanSuccessful && srFilenameUnicodeResult.bBlock) ||
				(srFilenameAsciiResult.bScanSuccessful && srFilenameAsciiResult.bBlock) || 
				(srParentFilenameUnicodeResult.bScanSuccessful && srParentFilenameUnicodeResult.bBlock) ||
				(srParentFilenameAsciiResult.bScanSuccessful && srParentFilenameAsciiResult.bBlock)
				);
	
		bool bQuarantine = (srCommandLineAsciiResult.bScanSuccessful && srCommandLineAsciiResult.bQuarantine) ||
			(srCommandLineUnicodeResult.bScanSuccessful && srCommandLineUnicodeResult.bQuarantine) ||
			(srFilenameUnicodeResult.bScanSuccessful && srFilenameUnicodeResult.bQuarantine) ||
			(srFilenameAsciiResult.bScanSuccessful && srFilenameAsciiResult.bQuarantine) ||
			(srParentFilenameUnicodeResult.bScanSuccessful && srParentFilenameUnicodeResult.bQuarantine) ||
			(srParentFilenameAsciiResult.bScanSuccessful && srParentFilenameAsciiResult.bQuarantine);

		bool bLog = g_AlwaysLog || bBlockProcess || bQuarantine || 
			(srCommandLineAsciiResult.bScanSuccessful && srCommandLineAsciiResult.bLog) ||
			(srCommandLineUnicodeResult.bScanSuccessful && srCommandLineUnicodeResult.bLog) ||
			(srFilenameUnicodeResult.bScanSuccessful && srFilenameUnicodeResult.bLog) ||
			(srFilenameAsciiResult.bScanSuccessful && srFilenameAsciiResult.bLog) ||
			(srParentFilenameUnicodeResult.bScanSuccessful && srParentFilenameUnicodeResult.bLog) ||
			(srParentFilenameAsciiResult.bScanSuccessful && srParentFilenameAsciiResult.bLog);
		void (*LogFn)(const char *, ...) = bBlockProcess ? e->LogCriticalFmt : e->LogFmt;
		if (bLog) {
			LogFn(
				"\n" \
				"EventType:ProcessCreate\n" \
				"Process:%ls\n" \
				"Username:%ls\n" \
				"Groupname:%ls\n" \
				"PID:%u\n" \
				"MD5:%s\n" \
				"SHA1:%s\n" \
				"SHA256:%s\n" \
				"CommandLine:%ls\n" \
				"CommandLineAsciiRuleBlock:%ls\n" \
				"CommandLineUnicodeRuleBlock:%ls\n" \
				"CommandLineAsciiRuleQuarantine:%ls\n" \
				"CommandLineUnicodeRuleQuarantine:%ls\n" \
				"CommandLineAsciiRuleLog:%ls\n" \
				"CommandLineUnicodeRuleLog:%ls\n" \
				"FilenameAsciiRuleBlock:%ls\n" \
				"FilenameUnicodeRuleBlock:%ls\n" \
				"FilenameAsciiRuleQuarantine:%ls\n" \
				"FilenameUnicodeRuleQuarantine:%ls\n" \
				"FilenameAsciiRuleLog:%ls\n" \
				"FilenameUnicodeRuleLog:%ls\n" \
				"ParentFilenameAsciiRuleBlock:%ls\n" \
				"ParentFilenameUnicodeRuleBlock:%ls\n" \
				"ParentFilenameAsciiRuleQuarantine:%ls\n" \
				"ParentFilenameUnicodeRuleQuarantine:%ls\n" \
				"ParentFilenameAsciiRuleLog:%ls\n" \
				"ParentFilenameUnicodeRuleLog:%ls\n" \
				"ParentPID:%u\n" \
				"ParentName:%ls\n" \
				"HashBlacklisted:%s\n" \
				"FilenameBlacklisted:%s\n" \
				"UsernameBlacklisted:%s\n" \
				"GroupnameBlacklisted:%s\n" \
				"Quarantine:%s\n" \
				"Block:%s\n" \
				"",
				e->lpszFileName,
				username.c_str(),
				groupname.c_str(),
				e->dwProcessId,
				(g_HashExes && bHashSuccessful) ? hashes.md5_hexdigest : "*DISABLED*",
				(g_HashExes && bHashSuccessful) ? hashes.sha1_hexdigest : "*DISABLED*",
				(g_HashExes && bHashSuccessful) ? hashes.sha256_hexdigest : "*DISABLED*",
				g_LogCommandLine ? lpszCommandLine : L"*DISABLED*",
				tg_CommandLineRulesContext ? (srCommandLineAsciiResult.bScanSuccessful ? srCommandLineAsciiResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_CommandLineRulesContext ? (srCommandLineUnicodeResult.bScanSuccessful ? srCommandLineUnicodeResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_CommandLineRulesContext ? (srCommandLineAsciiResult.bScanSuccessful ? srCommandLineAsciiResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_CommandLineRulesContext ? (srCommandLineUnicodeResult.bScanSuccessful ? srCommandLineUnicodeResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_CommandLineRulesContext ? (srCommandLineAsciiResult.bScanSuccessful ? srCommandLineAsciiResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_CommandLineRulesContext ? (srCommandLineUnicodeResult.bScanSuccessful ? srCommandLineUnicodeResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameAsciiResult.bScanSuccessful ? srFilenameAsciiResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameUnicodeResult.bScanSuccessful ? srFilenameUnicodeResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameAsciiResult.bScanSuccessful ? srFilenameAsciiResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameUnicodeResult.bScanSuccessful ? srFilenameUnicodeResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameAsciiResult.bScanSuccessful ? srFilenameAsciiResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_FilenameRulesContext ? (srFilenameUnicodeResult.bScanSuccessful ? srFilenameUnicodeResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameAsciiResult.bScanSuccessful ? srParentFilenameAsciiResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameUnicodeResult.bScanSuccessful ? srParentFilenameUnicodeResult.szBlockRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameAsciiResult.bScanSuccessful ? srParentFilenameAsciiResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameUnicodeResult.bScanSuccessful ? srParentFilenameUnicodeResult.szQuarantineRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameAsciiResult.bScanSuccessful ? srParentFilenameAsciiResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				tg_ParentFilenameRulesContext ? (srParentFilenameUnicodeResult.bScanSuccessful ? srParentFilenameUnicodeResult.szLogRuleNames : L"*FAILED*") : L"*SKIPPED*",
				e->dwParentProcessId,
				szParentName,
				bHashBlacklisted ? "Yes" : "No",
				bFilenameBlacklisted ? "Yes" : "No",
				bUsernameBlacklisted ? "Yes" : "No",
				bGroupnameBlacklisted ? "Yes" : "No",
				bQuarantine ? "Yes" : "No",
				bBlockProcess ? "Yes" : "No"
				);
		}

		if (bQuarantine) e->QuarantineFile(e->lpszFileName, g_MaxQuarantineFileSize, NULL, 0);

		if (bBlockProcess) {
			dwResultFlags = PROCFILTER_RESULT_BLOCK_PROCESS;
		}
	} else if (e->dwEventId == PROCFILTER_EVENT_PROCESS_TERMINATE) {
		EnterCriticalSection(&g_cs);
		auto iter = g_WhitelistedPids.find(e->dwProcessId);
		if (iter != g_WhitelistedPids.end()) g_WhitelistedPids.erase(iter);
		LeaveCriticalSection(&g_cs);
	} else if (e->dwEventId == PROCFILTER_EVENT_THREAD_CREATE) {
		if (g_LogRemoteThreads) {
			if (e->dwParentProcessId != e->dwProcessId) {
				// Skip whitelisted parents
				EnterCriticalSection(&g_cs);
				auto iter = g_WhitelistedPids.find(e->dwProcessId);
				bool bWhitelisted = iter != g_WhitelistedPids.end();
				LeaveCriticalSection(&g_cs);
				if (bWhitelisted) return PROCFILTER_RESULT_NONE;

				// Restrict remote thread interruption to only unprivileged threads since suspending/blocking
				// some system threads can lead to blue screens.
				HANDLE hNewPid = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, e->dwProcessId);
				bool bIsElevated = false;
				// On the other hand we aren't blocking here and the response time should be "fast" enough to where it doesn't matter
				//if (hNewPid && e->IsElevated(hNewPid, &bIsElevated) && !bIsElevated) {
				if (hNewPid) {
					WCHAR szSource[MAX_PATH + 1];
					WCHAR szTarget[MAX_PATH + 1];

					ULONG64 ulProcessTime = 0;
					FILETIME ftUnused;
					FILETIME ftUserTime;

					// Make sure this is not the first thread in the process
					if (GetProcessTimes(hNewPid, &ftUnused, &ftUnused, &ftUnused, &ftUserTime) && (ftUserTime.dwHighDateTime > 0 || ftUserTime.dwLowDateTime > 0)) {
						if (e->GetProcessFileName(e->dwParentProcessId, szSource, sizeof(szSource)) && e->GetProcessFileName(e->dwProcessId, szTarget, sizeof(szTarget))) {
							const WCHAR *lpszSourceBaseName = e->GetProcessBaseNamePointer(szSource);
							const WCHAR *lpszTargetBaseName = e->GetProcessBaseNamePointer(szTarget);

							e->LogWarningFmt(
								"\n" \
								"EventType:RemoteThreadCreate\n" \
								"ThreadID:%u\n" \
								"SourceBasename:%ls\n" \
								"Source:%ls\n" \
								"SourcePID:%u\n" \
								"TargetBasename:%ls\n"
								"Target:%ls\n" \
								"TargetPID:%u\n" \
								"",
								e->dwThreadId,
								lpszSourceBaseName,
								szSource,
								e->dwParentProcessId,
								lpszTargetBaseName,
								szTarget,
								e->dwProcessId
								);
						}
					}

					CloseHandle(hNewPid);
				}
			}
		}
	} else if (e->dwEventId == PROCFILTER_EVENT_IMAGE_LOAD) {
		if (e->lpszFileName) {

			// Don't hash DLL loads for whitelisted processes
			EnterCriticalSection(&g_cs);
			auto iter = g_WhitelistedPids.find(e->dwProcessId);
			bool bWhitelisted = iter != g_WhitelistedPids.end();
			LeaveCriticalSection(&g_cs);
			if (bWhitelisted) return PROCFILTER_RESULT_DONT_SCAN;

			// Filename whitelisted?
			bool bFilenameWhitelisted = false;
			if (!g_WhitelistExceptions.matchesFilename(e->lpszFileName)) {
				bFilenameWhitelisted = g_Whitelist.matchesFilename(e->lpszFileName);
				if (bFilenameWhitelisted) return PROCFILTER_RESULT_DONT_SCAN;
			}

			// Filename blacklisted?
			bool bFilenameBlacklisted = g_Blacklist.matchesFilename(e->lpszFileName);

			// Hashes whitelisted?
			bool bHashBlacklisted = false;
			bool bHashSuccessful = false;
			HASHES hashes;
			if (g_HashDlls) {
				bHashSuccessful = e->HashFile(e->lpszFileName, g_MaxHashFileSize, &hashes);
				if (bHashSuccessful) {
					if (!g_WhitelistExceptions.containsAnyHash(&hashes)) {
						if (g_Whitelist.containsAnyHash(&hashes)) return PROCFILTER_RESULT_DONT_SCAN;
					}

					// Hashes blacklisted
					bHashBlacklisted = g_Blacklist.containsAnyHash(&hashes);
				}
			}

			bool bBlock = bHashBlacklisted || bFilenameBlacklisted;
			bool bQuarantine = bBlock;

			WCHAR szProcessName[MAX_PATH + 1];
			e->GetProcessFileName(e->dwProcessId, szProcessName, sizeof(szProcessName));

			void(*LogFn)(const char *, ...) = bBlock ? e->LogCriticalFmt : e->LogFmt;
			bool bLog = g_LogLoadedDllNames || g_HashDlls;
			if (bLog) {
				LogFn(
				"\n" \
				"EventType:DllLoad\n" \
				"ProcessName:%ls\n" \
				"DllName:%ls\n" \
				"PID:%u\n" \
				"MD5:%s\n" \
				"SHA1:%s\n" \
				"SHA256:%s\n" \
				"HashBlacklisted:%s\n" \
				"FilenameBlacklisted:%s\n" \
				"Quarantine:%s\n" \
				"Blocked:%s\n" \
				"",
				szProcessName,
				e->lpszFileName,
				e->dwProcessId,
				(g_HashDlls && bHashSuccessful) ? hashes.md5_hexdigest : "*DISABLED*",
				(g_HashDlls && bHashSuccessful) ? hashes.sha1_hexdigest : "*DISABLED*",
				(g_HashDlls && bHashSuccessful) ? hashes.sha256_hexdigest : "*DISABLED*",
				bHashBlacklisted ? "Yes" : "No",
				bFilenameBlacklisted ? "Yes" : "No",
				bQuarantine ? "Yes" : "No",
				bBlock ? "Yes" : "No"
				);
			}

			if (bQuarantine) e->QuarantineFile(e->lpszFileName, g_MaxQuarantineFileSize, NULL, 0);

			if (bBlock) {
				dwResultFlags = PROCFILTER_RESULT_BLOCK_PROCESS;
			}
		}
	}

	return dwResultFlags;
}
