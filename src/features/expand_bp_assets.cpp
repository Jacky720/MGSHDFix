#include "stdafx.h"
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#include "expand_bp_assets.hpp"

#include "common.hpp"
#include "logging.hpp"

typedef long long (__fastcall* FASTCALL_1IN1OUT)(void*);
typedef void* (__fastcall* FASTCALL_3IN1OUT)(void*, void*, size_t);

// core components of struct; original definition in system/libfs/loader_flatfs.h
typedef struct {
	unsigned int loadState;
	char manifestPath[260];
	char* manifestBuffer;
	char field_110[8]; // irrelevant
	char bpAssetsPath[260];
	char field_21C[4]; // padding
	char* bpAssetsBuffer;
	char field_228[16 + 260 * 3 + 4]; // irrelevant
	// begin sub-struct "pManifestFileState"/"pBPAssetsFileState"
	void* currentFileHandle;
	void* operatingFileHandle; // duplicate?
	char* currentFileBuffer; // only used for individual cmdl/ctxr files
	size_t currentFileSize;
} BPLoadFileState;

namespace {
	FASTCALL_1IN1OUT BPOpenFile;
	FASTCALL_1IN1OUT BPGetFileSize;
	FASTCALL_3IN1OUT BPReadFile;
	FASTCALL_1IN1OUT BPCloseFile;
	FASTCALL_1IN1OUT BPIsReadDone;
	SafetyHookInline OpenHook{};
	SafetyHookInline FileSizeHook{};
	SafetyHookInline ReadHook{};
	SafetyHookInline ReadAsyncHook{};
	SafetyHookInline CloseHook{};
	SafetyHookInline FinishHook{};
}

// Synchronous file operation functions to substitute

#define debug(...) do { if (g_Logging.bVerboseLogging) { spdlog::info("BP_FilesysChanges: " __VA_ARGS__); } } while (0)
#define HOOK_BP_FILESYS
//#define LOG_ONLY

void* __fastcall HD_OpenFile(const char* path) {
	debug("Opening {}", std::string(path));
#ifdef LOG_ONLY
	return OpenHook.call<void*>(path);
#else
	return fopen(path, "rb");
#endif
}

void __fastcall HD_CloseFile(void* const fp) {
	debug("Closing {}", (long)fp);
#ifdef LOG_ONLY
	CloseHook.call<void>(fp);
#else
	fclose((FILE*)fp);
#endif
}

long __fastcall HD_GetFileSize(void* const fp) {
#ifdef LOG_ONLY
	long sz = FileSizeHook.call<long>(fp);
#else
	size_t oldpos = ftell((FILE*)fp);
	fseek((FILE*)fp, 0, SEEK_END);
	long sz = ftell((FILE*)fp);
	fseek((FILE*)fp, oldpos, SEEK_SET);
#endif
	debug("size of {} is {}", (long)fp, sz);
	return sz;
}

// Used for both ReadFile and ReadFileAsync, only difference is ReadFile technically should discard rax (return type void)
void* __fastcall HD_ReadFile(void* const fp, void* const buf, const size_t sz) {
	debug("reading {} bytes from {} into {}", sz, (long)fp, (long)buf);
#ifdef LOG_ONLY
	return ReadHook.call<void*>(fp, buf, sz);
#else
	fread(buf, sz, 1, (FILE*)fp);
	return fp;
#endif
}

// "Is operation complete?" Yes, obviously; it was done synchronously with fread.
int __fastcall HD_TryFinishFileOp() {
	debug("file op is synchronous");
#ifdef LOG_ONLY
	return FinishHook.call<int>();
#else
	return 1;
#endif
}


// Helper for optimized file buffer allocation
static inline size_t RoundUp(size_t size) {
	int ret = 1;
	while (ret < size)
		ret <<= 1;
	return ret;
}

// Key on the cache fields, not the source path. One texture can be registered against
// several tri groups, and those lines differ only in the last field.
static inline std::string_view CacheKey(const std::string& line) {
	const size_t comma = line.find(',');
	return comma == std::string::npos ? std::string_view(line) : std::string_view(line).substr(comma + 1);
}

static void SplitLines(const char* data, size_t size, std::vector<std::string>& out) {
	size_t start = 0;
	for (size_t i = 0; i <= size; i++) {
		if (i != size && data[i] != '\n') {
			continue;
		}
		size_t end = i;
		while (end > start && (data[end - 1] == '\r' || data[end - 1] == '\n')) {
			end--;
		}
		if (end > start) {
			out.emplace_back(data + start, end - start);
		}
		start = i + 1;
	}
}

// Ultimate ASI Loader redirects opens into its overload folder, but these lists are found
// by scanning a directory rather than opened by name, so a mod's copies there are never
// seen. Ask the loader where that folder is and scan it too. No loader, no change.
typedef bool (WINAPI* GetOverloadPathWFn)(wchar_t*, size_t);

static const std::filesystem::path& OverloadRoot() {
	static std::filesystem::path root;
	static bool resolved = false;
	if (resolved) {
		return root;
	}
	resolved = true;

	HMODULE mods[256];
	DWORD needed = 0;
	if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
		const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(mods));
		for (size_t i = 0; i < count; i++) {
			auto fn = (GetOverloadPathWFn)GetProcAddress(mods[i], "GetOverloadPathW");
			if (!fn) {
				continue;
			}
			wchar_t buf[MAX_PATH] {};
			if (fn(buf, std::size(buf)) && buf[0]) {
				root = buf;
				spdlog::info("BP_FilesysChanges: loader overload folder is {}", root.string());
			}
			break;
		}
	}
	return root;
}

// The same stage folder inside the overload root, or empty when there is no loader.
static std::filesystem::path OverloadMirror(const std::filesystem::path& directory) {
	const std::filesystem::path& root = OverloadRoot();
	if (root.empty()) {
		return {};
	}
	std::error_code ec;
	std::filesystem::path rel = std::filesystem::relative(directory, sExePath.parent_path(), ec);
	if (ec || rel.empty() || *rel.begin() == "..") {
		return {};
	}
	return root / rel;
}

static inline void* LoadSimilarFiles(BPLoadFileState* state, bool isBPAssets) {
#define match_substr (isBPAssets ? "bp_assets_" : "manifest_")
	debug("Now searching for other {} files", match_substr);
	// Find files similar to the currently used path.
	const char* filePath = isBPAssets ? state->bpAssetsPath : state->manifestPath;
	std::filesystem::path directory = std::filesystem::path(filePath).parent_path();
	char** buffer = isBPAssets ? &state->bpAssetsBuffer : &state->manifestBuffer;

	// Avoid realloc when reasonable
	size_t prevBufferSize = state->currentFileSize + 1;
	// For after the loop
	size_t originalFileSize = state->currentFileSize;

	std::vector<std::filesystem::path> entries;
	std::error_code ec;
	for (auto const& d : { directory, OverloadMirror(directory) }) {
		if (d.empty() || !std::filesystem::is_directory(d, ec)) {
			continue;
		}
		for (auto const& dir_entry : std::filesystem::directory_iterator(d, ec)) {
			if (dir_entry.is_regular_file(ec)) {
				entries.push_back(dir_entry.path());
			}
		}
	}

	for (auto const& entry_path : entries) {
		if (entry_path.stem().string().starts_with(match_substr)
			&& entry_path.extension() == ".txt") {
			// Match found, load it
			// Need to update: buffer, file size, file handle
			BPCloseFile(state->currentFileHandle);
			state->currentFileHandle = (void*)BPOpenFile((void*)entry_path.string().c_str());
			size_t newFileSize = BPGetFileSize(state->currentFileHandle);
			size_t newBufferSize = RoundUp(state->currentFileSize + newFileSize + 1);
			if (newBufferSize > prevBufferSize) {
				char* oldBuf = *buffer;
				*buffer = (char*)realloc(oldBuf, newBufferSize);
				//spdlog::info("buffer for {} was at {}, now {}", filePath, (long long)oldBuf, (long long)*buffer);
				(*buffer)[state->currentFileSize + newFileSize] = '\0';
			}
			prevBufferSize = newBufferSize;
			// Oh, and read the new file, of course.
			//state->currentFileHandle = BPReadFile(state->currentFileHandle, &(*buffer)[state->currentFileSize], newFileSize);
			// The Master Collection file reader is asynchronous. We need more consistency than that.
			FILE* fp = fopen(entry_path.string().c_str(), "rb");
			fread(&(*buffer)[state->currentFileSize], newFileSize, 1, fp);
			fclose(fp);
			if ((*buffer)[state->currentFileSize] == '\0')
			{
				// ??? mission failed? what?
				spdlog::warn("Failed to load a text file ({} supplementing {}), the simplest file load possible?", entry_path.string(), filePath);
				continue;
			}
			state->currentFileSize += newFileSize;
		}
	}
	
	// Merge instead of prepending. A cache path the stage already lists replaces it in
	// place, since loading an asset twice starves the streamer and runs the stage in slow
	// motion. New entries go on the end: the list is in dependency order.
	size_t newFilesSize = state->currentFileSize - originalFileSize;
	if (newFilesSize) {
		std::vector<std::string> lines, extra;
		SplitLines(*buffer, originalFileSize, lines);
		SplitLines(&(*buffer)[originalFileSize], newFilesSize, extra);

		size_t replaced = 0, added = 0;
		for (const std::string& line : extra) {
			const std::string_view key = CacheKey(line);
			auto match = std::find_if(lines.begin(), lines.end(),
				[&](const std::string& v) { return CacheKey(v) == key; });
			if (match != lines.end()) {
				*match = line;
				replaced++;
			}
			else {
				lines.push_back(line);
				added++;
			}
		}

		std::string merged;
		merged.reserve(state->currentFileSize + 2 * lines.size() + 1);
		for (const std::string& line : lines) {
			merged += line;
			merged += "\r\n";
		}

		const size_t needed = RoundUp(merged.size() + 1);
		if (needed > prevBufferSize) {
			*buffer = (char*)realloc(*buffer, needed);
		}
		memcpy(*buffer, merged.data(), merged.size());
		(*buffer)[merged.size()] = '\0';
		state->currentFileSize = merged.size();
		spdlog::info("{}: merged {} supplementary entries ({} replaced, {} added)",
			std::filesystem::path(filePath).filename().string(), extra.size(), replaced, added);
	}

	return state->currentFileHandle;
}


void BP_FilesysChanges::Initialize() {

	if (!(eGameType & MGS2)) // TODO: MGS3 support, likely requires different patterns
	{
		return;
	}

	if (Util::IsSteamOS()) // temporary. linux filesystems are stupid and shit reliant on expand_bp_assets can randomly cause crashing.
	{
        spdlog::warn("BP_FilesysChanges: Unstable on SteamOS due to crashing issues with Linux filesystems.");
        //spdlog::warn("BP_FilesysChanges: Features reliant on expand_bp_assets will not be available.");
        //spdlog::warn("BP_FilesysChanges: This includes: Snake Holster Fix, Hostage Arm Fix, Shell 1 Core Camera Screen fix, Alternative Colonel MSX Sprite.");
		//return;
	}

	bLoaded = true;

	// The HD Collection (hence, the Master Collection) have a different file system to the original games.
	// Files are stored with their proper names, but loaded into a cache with their strcode names as needed.
	// This cache is defined by manifest.txt and bp_assets.txt files for each stage and each codec character.
	// To increase compatibility, we hook the cache loader and allow loading multiple such manifests.
	// Specifically, the part of the loader that initializes a buffer and reads the file into memory.
	//uint8_t* ManifestLoader = Memory::PatternScan(baseModule, "40 53 48 83 EC ?? 48 8B D9 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 4B", "manifest.txt loader");
	uint8_t* BPAssetsLoader = Memory::PatternScan(baseModule, "40 53 55 56 57 41 54 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24", "bp_assets.txt loader");
	if (!BPAssetsLoader) {
		spdlog::error("Failed to match BP_LoadFlatFSSync.");
		return;
	}

	BPIsReadDone = (FASTCALL_1IN1OUT)Memory::GetRelativeOffset(BPAssetsLoader + 0x57);
	BPCloseFile = (FASTCALL_1IN1OUT)Memory::GetRelativeOffset(BPAssetsLoader + 0x67);
	BPOpenFile = (FASTCALL_1IN1OUT)Memory::GetRelativeOffset(BPAssetsLoader + 0x91);
	BPGetFileSize = (FASTCALL_1IN1OUT)Memory::GetRelativeOffset(BPAssetsLoader + 0xa6);
	BPReadFile = (FASTCALL_3IN1OUT)Memory::GetRelativeOffset(BPAssetsLoader + 0x10f);


	// Both these injections are immediately after the file is read in; we can close the handle and open new ones as needed.
	// system/libfs/loader_flatfs.cpp -> BP_BeginLoadFlatFS()
	{
		MAKE_HOOK_MID(baseModule, "48 89 83 50 05 00 00 48 83 C4 20 5B C3", "manifest.txt Union", {
			// rax = file handle
			// rbx = load state struct
			ctx.rax = (uintptr_t)LoadSimilarFiles((BPLoadFileState*)ctx.rbx, false);
		});
	}
	// system/libfs/loader_flatfs.cpp -> BP_LoadFlatFSSync()
	{
		MAKE_HOOK_MID(baseModule, "48 89 87 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B 8F ?? ?? ?? ?? E8", "bp_assets.txt Union", {
			// rax = file handle
			// rdi = load state struct
			ctx.rax = (uintptr_t)LoadSimilarFiles((BPLoadFileState*)ctx.rdi, true);
		});
	}

	// Various hooks in bp/shared/BP_FileSupport.h (and whichever CPP file the Master Collection builds it with)
	// to use standard synchronous functions for file management (should fix realloc() above on Linux)
#ifdef HOOK_BP_FILESYS
	{
		// BP_OpenFile
		// 48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 50 48 8b 1d //?? ?? ?? ?? 48 8b f1 48 85 db
		OpenHook = safetyhook::create_inline(reinterpret_cast<void*>(BPOpenFile), reinterpret_cast<void*>(HD_OpenFile));
		// BP_CloseFile
		// 48 89 5c 24 08 57 48 83 ec 20 48 8b 1d ?? ?? ?? ?? 48 8b f9 48 85
		CloseHook = safetyhook::create_inline(reinterpret_cast<void*>(BPCloseFile), reinterpret_cast<void*>(HD_CloseFile));
		// BP_GetFileSize
		// 40 53 48 83 ec 20 48 8b d9 e8 ?? ?? ?? ?? 48 83 7b 08 00
		FileSizeHook = safetyhook::create_inline(reinterpret_cast<void*>(BPGetFileSize), reinterpret_cast<void*>(HD_GetFileSize));
		// BP_ReadFile
		// 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b 1d ?? ?? ?? ?? 49 8b f0 48 8b ea ...
		ReadHook = safetyhook::create_inline(reinterpret_cast<void*>(BPReadFile), reinterpret_cast<void*>(HD_ReadFile));
		// BP_ReadFileAsync is very nearly byte-for-byte the same as ReadFile, so do ReadFile first... then pattern match
		// Also, it's the same synchronous solution as ReadFile.
		// 48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 48 83 ec 20 48 8b 1d ?? ?? ?? ?? 49 8b f0 48 8b ea ...
		uint8_t* BPReadAsync = Memory::PatternScan(baseModule, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 49 8B F0 48 8B EA", "BP_ReadFileAsync");
		if (!BPReadAsync) {
			spdlog::error("Failed to match BP_ReadFileAsync, may cause issues.");
		}
		ReadAsyncHook = safetyhook::create_inline(reinterpret_cast<void*>(BPReadAsync), reinterpret_cast<void*>(HD_ReadFile));
		// BP_TryFinishFileOp
		// 40 57 48 83 ec 20 48 8b f9 e8 ?? ?? ?? ?? 84 c0
		FinishHook = safetyhook::create_inline(reinterpret_cast<void*>(BPIsReadDone), reinterpret_cast<void*>(HD_TryFinishFileOp));
	}
#endif
}
