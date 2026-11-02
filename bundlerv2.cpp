#ifdef DEBUG
#define NOMINMAX
#endif // DEBUG
#include <windows.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <vector>
#include <optional>

namespace chrono = std::chrono;
namespace fs = std::filesystem;
using json = nlohmann::json;
using steady_clock = chrono::steady_clock;
using time_point = steady_clock::time_point;
using string = std::string;
using path = fs::path;

const string MainFoldersToCopy[3] = { "Assets", "Libraries", "Guis" };

static double Elasped(auto Start) {
	return chrono::duration_cast<chrono::duration<double, std::milli>>(steady_clock::now() - Start).count();
}

static double RoundElapsed(double Value) {
	return Value < 1 ? std::round(Value * 10) / 10 : std::round(Value);
}

static string GetBuildName(const string& Name) {
	const size_t Pos = Name.find(" - ");
	if (Pos == string::npos) return Name;

	return Name.substr(0, Pos) + ".lua";
}

struct CopyTask {
	path Source;
	path Destination;
	DWORD SourceSizeHigh = 0;
	DWORD SourceSizeLow = 0;
	FILETIME SourceWriteTime{};
};

struct WorkItem {
	string Name;
	std::vector<CopyTask> Files;
	double ElapsedMs = 0.0;
	bool Log = false;
};

struct FileMetadata {
	DWORD SizeHigh = 0;
	DWORD SizeLow = 0;
	FILETIME WriteTime{};
};

struct FileHandle {
	HANDLE Value = INVALID_HANDLE_VALUE;

	explicit FileHandle(HANDLE Handle) : Value(Handle) {}
	~FileHandle() {
		if (Value != INVALID_HANDLE_VALUE) {
			CloseHandle(Value);
		}
	}

	FileHandle(const FileHandle&) = delete;
	FileHandle& operator=(const FileHandle&) = delete;

	bool IsValid() const {
		return Value != INVALID_HANDLE_VALUE;
	}
};

static void EnsureDirectory(const path& Path) {
	const DWORD Attributes = GetFileAttributesW(Path.c_str());
	if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return;
	}

	std::error_code Error;
	fs::create_directories(Path, Error);
	if (Error) {
		throw fs::filesystem_error("Failed to create directory", Path, Error);
	}
}

static void CollectFiles(const path& Directory, bool Recursive, std::unordered_map<std::wstring, FileMetadata>& Files) {
	std::wstring Pattern = Directory.native();
	if (!Pattern.empty() && Pattern.back() != L'\\' && Pattern.back() != L'/') {
		Pattern.push_back(L'\\');
	}
	Pattern.push_back(L'*');

	WIN32_FIND_DATAW Data{};
	HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

	if (Find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
		Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, 0);
	}

	if (Find == INVALID_HANDLE_VALUE) return;

	do {
		const std::wstring Name = Data.cFileName;
		if (Name == L"." || Name == L"..") continue;

		const bool IsDirectory = (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (IsDirectory) {
			if (Recursive && (Data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
				CollectFiles(Directory / Name, true, Files);
			}
			continue;
		}

		Files.emplace(
			(Directory / Name).native(),
			FileMetadata{ Data.nFileSizeHigh, Data.nFileSizeLow, Data.ftLastWriteTime }
		);
	} while (FindNextFileW(Find, &Data));

	FindClose(Find);
}

static void CollectDestinationFiles(const path& Destination, std::unordered_map<std::wstring, FileMetadata>& Files) {
	CollectFiles(Destination, false, Files);
	CollectFiles(Destination / L"Assets", true, Files);
	CollectFiles(Destination / L"Libraries", true, Files);
	CollectFiles(Destination / L"Guis", true, Files);
	CollectFiles(Destination / L"Games", false, Files);
}

enum class ScanMode {
	MainFolder,
	Games
};

static bool IsMainFolderW(const std::wstring& Name) {
	return Name == L"Assets" || Name == L"Libraries" || Name == L"Guis";
}

static CopyTask MakeCopyTask(const WIN32_FIND_DATAW& Data, const path& Source, const path& Destination) {
	CopyTask Task;
	Task.Source = Source;
	Task.Destination = Destination;
	Task.SourceSizeHigh = Data.nFileSizeHigh;
	Task.SourceSizeLow = Data.nFileSizeLow;
	Task.SourceWriteTime = Data.ftLastWriteTime;
	return Task;
}

static void AddTasksDirectory(std::vector<WorkItem>& Work, std::unordered_map<std::wstring, size_t>& GameIndex, const path& Source, const path& Destination, const path& RelativeDir, ScanMode Mode) {
	const path FullDir = RelativeDir.empty() ? Source : Source / RelativeDir;
	std::wstring Pattern = FullDir.native();
	if (!Pattern.empty() && Pattern.back() != L'\\' && Pattern.back() != L'/') {
		Pattern.push_back(L'\\');
	}
	Pattern.push_back(L'*');

	WIN32_FIND_DATAW Data{};
	HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

	if (Find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
		Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, 0);
	}

	if (Find == INVALID_HANDLE_VALUE) return;

	do {
		const std::wstring Name = Data.cFileName;
		if (Name == L"." || Name == L"..") continue;

		const bool IsDirectory = (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		const path Relative = RelativeDir.empty() ? path(Name) : RelativeDir / Name;

		if (IsDirectory) {
			if ((Data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

			if (Mode == ScanMode::MainFolder) {
				EnsureDirectory(Destination / Relative);
				AddTasksDirectory(Work, GameIndex, Source, Destination, Relative, ScanMode::MainFolder);
			}
			else {
				AddTasksDirectory(Work, GameIndex, Source, Destination, Relative, ScanMode::Games);
			}
			continue;
		}

		if (Mode == ScanMode::MainFolder) {
			Work.push_back({ "", {MakeCopyTask(Data, Source / Relative, Destination / Relative)}, 0.0, false });
		}
		else {
			const size_t NameLength = Name.size();
			if (NameLength < 4 || Name.compare(NameLength - 4, 4, L".lua") != 0) {
				continue;
			}

			const path GameDirectory = Relative.parent_path();
			const path Target = Destination / L"Games" / GetBuildName(path(Name).string());
			const CopyTask Task = MakeCopyTask(Data, Source / Relative, Target);

			if (GameDirectory == L"Games") {
				Work.push_back({ "", {Task}, 0.0, false });
			}
			else {
				const std::wstring Key = GameDirectory.native();
				const size_t NewIndex = Work.size();
				auto [Entry, Inserted] = GameIndex.try_emplace(Key, NewIndex);

				size_t Index = NewIndex;
				if (Inserted) {
					Work.push_back({ GameDirectory.filename().string(), {}, 0.0, true });
				}
				else {
					Index = Entry->second;
				}

				Work[Index].Files.push_back(Task);
			}
		}
	} while (FindNextFileW(Find, &Data));

	FindClose(Find);
}

static std::vector<WorkItem> ScanMainFolder(const path& Source, const path& Destination, const path& Relative) {
	std::vector<WorkItem> Work;
	std::unordered_map<std::wstring, size_t> GameIndex;
	AddTasksDirectory(Work, GameIndex, Source, Destination, Relative, ScanMode::MainFolder);
	return Work;
}

static std::vector<WorkItem> ScanCategory(const path& Source, const path& Destination, const path& Relative) {
	std::vector<WorkItem> Work;
	std::unordered_map<std::wstring, size_t> GameIndex;
	AddTasksDirectory(Work, GameIndex, Source, Destination, Relative, ScanMode::Games);
	return Work;
}

static void AppendWork(std::vector<WorkItem>& Work, std::vector<WorkItem>&& Items) {
	Work.insert(Work.end(), std::make_move_iterator(Items.begin()), std::make_move_iterator(Items.end()));
}

static void AddTasks(std::vector<WorkItem>& Work, const path& Source, const path& Destination) {
	Work.reserve(64);

	EnsureDirectory(Destination);

	for (const string& Folder : MainFoldersToCopy) {
		EnsureDirectory(Destination / Folder);
	}

	EnsureDirectory(Destination / "Games");

	std::vector<path> MainFolders;
	std::vector<path> Categories;
	std::vector<CopyTask> RootFiles;
	std::vector<CopyTask> GamesRootFiles;

	auto Enumerate = [](const path& Directory, auto&& Callback) {
		std::wstring Pattern = Directory.native();
		if (!Pattern.empty() && Pattern.back() != L'\\' && Pattern.back() != L'/') {
			Pattern.push_back(L'\\');
		}
		Pattern.push_back(L'*');

		WIN32_FIND_DATAW Data{};
		HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

		if (Find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
			Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Data, FindExSearchNameMatch, nullptr, 0);
		}

		if (Find == INVALID_HANDLE_VALUE) {
			return;
		}

		do {
			const std::wstring Name = Data.cFileName;
			if (Name == L"." || Name == L"..") {
				continue;
			}
			Callback(Name, Data);
		} while (FindNextFileW(Find, &Data));

		FindClose(Find);
		};

	Enumerate(Source, [&](const std::wstring& Name, const WIN32_FIND_DATAW& Data) {
		if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			if (IsMainFolderW(Name)) {
				MainFolders.emplace_back(Name);
			}
		}
		else if (Name == L"Loader.lua") {
			RootFiles.push_back(MakeCopyTask(Data, Source / Name, Destination / L"Loader.lua"));
		}
		});

	Enumerate(Source / L"Games", [&](const std::wstring& Name, const WIN32_FIND_DATAW& Data) {
		if ((Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			Categories.emplace_back(path(L"Games") / Name);
		}
		else {
			const size_t NameLength = Name.size();
			if (NameLength >= 4 && Name.compare(NameLength - 4, 4, L".lua") == 0) {
				GamesRootFiles.push_back(MakeCopyTask(
					Data,
					Source / L"Games" / Name,
					Destination / L"Games" / GetBuildName(path(Name).string())
				));
			}
		}
		});

	for (const CopyTask& Task : RootFiles) {
		Work.push_back({ "", {Task}, 0.0, false });
	}
	for (const CopyTask& Task : GamesRootFiles) {
		Work.push_back({ "", {Task}, 0.0, false });
	}

	std::vector<std::future<std::vector<WorkItem>>> MainFutures;
	MainFutures.reserve(MainFolders.size());
	for (const path& Folder : MainFolders) {
		MainFutures.push_back(std::async(std::launch::async, [Source, Destination, Folder]() {
			return ScanMainFolder(Source, Destination, Folder);
			}));
	}

	std::vector<std::future<std::vector<WorkItem>>> CategoryFutures;
	CategoryFutures.reserve(Categories.size());
	for (const path& Category : Categories) {
		CategoryFutures.push_back(std::async(std::launch::async, [Source, Destination, Category]() {
			return ScanCategory(Source, Destination, Category);
			}));
	}

	for (auto& Future : MainFutures) {
		AppendWork(Work, Future.get());
	}
	for (auto& Future : CategoryFutures) {
		AppendWork(Work, Future.get());
	}
}

static bool IsUpToDate(const CopyTask& Task, const std::unordered_map<std::wstring, FileMetadata>& DestinationFiles) {
	const auto Entry = DestinationFiles.find(Task.Destination.native());
	if (Entry == DestinationFiles.end()) {
		return false;
	}

	const FileMetadata& Destination = Entry->second;
	return Task.SourceSizeHigh == Destination.SizeHigh && Task.SourceSizeLow == Destination.SizeLow && Task.SourceWriteTime.dwHighDateTime == Destination.WriteTime.dwHighDateTime && Task.SourceWriteTime.dwLowDateTime == Destination.WriteTime.dwLowDateTime;
}

static void CopyOne(const CopyTask& Task, const std::unordered_map<std::wstring, FileMetadata>& DestinationFiles) {
	if (IsUpToDate(Task, DestinationFiles)) return;

	FileHandle SourceHandle(CreateFileW(
		Task.Source.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr
	));

	if (!SourceHandle.IsValid()) {
		throw std::system_error(
			static_cast<int>(GetLastError()),
			std::system_category(),
			"Failed to open " + Task.Source.string()
		);
	}

	FILETIME WriteTime{};
	if (!GetFileTime(SourceHandle.Value, nullptr, nullptr, &WriteTime)) {
		throw std::system_error(
			static_cast<int>(GetLastError()),
			std::system_category(),
			"Failed to read timestamp of " + Task.Source.string()
		);
	}

	{
		FileHandle DestinationHandle(CreateFileW(
			Task.Destination.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		));

		if (!DestinationHandle.IsValid()) {
			throw std::system_error(
				static_cast<int>(GetLastError()),
				std::system_category(),
				"Failed to create " + Task.Destination.string()
			);
		}

		static thread_local std::vector<char> Buffer(1 << 20);

		for (;;) {
			DWORD Read = 0;
			if (!ReadFile(SourceHandle.Value, Buffer.data(), static_cast<DWORD>(Buffer.size()), &Read, nullptr)) {
				throw std::system_error(
					static_cast<int>(GetLastError()),
					std::system_category(),
					"Failed to read " + Task.Source.string()
				);
			}

			if (Read == 0) break;

			DWORD Written = 0;
			while (Written < Read) {
				DWORD Chunk = 0;
				if (!WriteFile(DestinationHandle.Value, Buffer.data() + Written, Read - Written, &Chunk, nullptr) || Chunk == 0) {
					throw std::system_error(
						static_cast<int>(GetLastError()),
						std::system_category(),
						"Failed to write " + Task.Destination.string()
					);
				}
				Written += Chunk;
			}
		}

		SetFileTime(DestinationHandle.Value, nullptr, nullptr, &WriteTime);
	}

	const DWORD Attributes = GetFileAttributesW(Task.Source.c_str());
	if (Attributes != INVALID_FILE_ATTRIBUTES) {
		SetFileAttributesW(Task.Destination.c_str(), Attributes & ~FILE_ATTRIBUTE_DIRECTORY);
	}
}

static void CopyFiles(std::vector<WorkItem>& Work, const std::unordered_map<std::wstring, FileMetadata>& DestinationFiles) {
	if (Work.empty()) return;

	bool AllUpToDate = true;
	for (const auto& Item : Work) {
		for (const auto& File : Item.Files) {
			if (!IsUpToDate(File, DestinationFiles)) {
				AllUpToDate = false;
				break;
			}
		}
		if (!AllUpToDate) break;
	}

	if (AllUpToDate) return;

	const unsigned int ThreadCount = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
	std::atomic<size_t> NextTask{ 0 };
	std::exception_ptr Failure;
	std::mutex FailureMutex;

	std::vector<std::thread> Workers;
	Workers.reserve(ThreadCount);

	for (unsigned int Index = 0; Index < ThreadCount; ++Index) {
		Workers.emplace_back([&] {
			for (;;) {
				const size_t TaskIndex = NextTask.fetch_add(1, std::memory_order_relaxed);
				if (TaskIndex >= Work.size()) {
					return;
				}

				const auto Start = steady_clock::now();
				try {
					for (const auto& File : Work[TaskIndex].Files) {
						CopyOne(File, DestinationFiles);
					}
					Work[TaskIndex].ElapsedMs = Elasped(Start);
				}
				catch (...) {
					std::lock_guard<std::mutex> Lock(FailureMutex);
					if (!Failure) {
						Failure = std::current_exception();
					}
					return;
				}
			}
			});
	}

	for (auto& Worker : Workers) {
		Worker.join();
	}

	if (Failure) {
		std::rethrow_exception(Failure);
	}
}

static void BuildToDestination(const path& Source, const path& Destination, int Index) {
	const bool Profile = GetEnvironmentVariableW(L"NARRATOR_PROFILE", nullptr, 0) > 0;

	std::cout << std::format("-- Destination #{} --\n", Index);

	const time_point DestinationStart = steady_clock::now();
	EnsureDirectory(Destination);

	std::unordered_map<std::wstring, FileMetadata> DestinationFiles;
	DestinationFiles.reserve(64);
	auto DestinationFuture = std::async(std::launch::async, [&Destination, &DestinationFiles]() {
		CollectDestinationFiles(Destination, DestinationFiles);
	});

	std::vector<WorkItem> Work;
	const time_point AddStart = steady_clock::now();
	AddTasks(Work, Source, Destination);
	const double AddMs = Elasped(AddStart);

	DestinationFuture.get();

	const time_point CopyStart = steady_clock::now();
	CopyFiles(Work, DestinationFiles);
	const double CopyMs = Elasped(CopyStart);

	const time_point LogStart = steady_clock::now();
	string Log;
	Log.reserve(Work.size() * 48);
	for (const auto& Item : Work) {
		if (Item.Log) {  Log += std::format("Built {} in {}ms\n", Item.Name,RoundElapsed(Item.ElapsedMs)); }
	}

	const double LogMs = Elasped(LogStart);
	const double BuildMs = Elasped(DestinationStart);
	const time_point PrintStart = steady_clock::now();
	std::cout << Log;
	std::cout << std::format("-- Built Destination #{} in {}ms --\n", Index, RoundElapsed(BuildMs));
	const double PrintMs = Elasped(PrintStart);

	if (Profile) {
		std::cerr << std::format("[profile #{}] add={} copy={} log={} print={} total={}\n", Index, AddMs, CopyMs, LogMs, PrintMs, Elasped(DestinationStart));
	}
}

int main() {
	std::ifstream ConfigFile("Config.json");

	if (!ConfigFile.is_open()) {
		std::cerr << "Failed to open Config.json\n";
		return 1;
	}

	const json Config = json::parse(ConfigFile);
	const path Source = Config["Source"].get<string>();
	const time_point Start = steady_clock::now();
	int Index = 0;

	std::vector<std::pair<path, int>> EnabledDestinations;

	// Build all destinations
	for (const auto& [Destination, Enabled] : Config["Destinations"].items()) {
		if (Enabled.get<bool>()) { Index++; EnabledDestinations.emplace_back(Destination, Index); }
	}

	if (EnabledDestinations.size() == 1) {
		BuildToDestination(Source, EnabledDestinations[0].first, EnabledDestinations[0].second);
	}
	else {
		std::vector<std::future<void>> Builds;
		Builds.reserve(EnabledDestinations.size());

		for (const auto& [Destination, CurrentIndex] : EnabledDestinations) {
			Builds.emplace_back(std::async(
				std::launch::async,
				[Source, Destination, CurrentIndex]() { BuildToDestination(Source, Destination, CurrentIndex); }
			));
		}

		// Display final result
		for (auto& Build : Builds) {
			Build.get();
		}
	}

	std::cout << std::format("Finished bundling all destinations in {}ms\n", RoundElapsed(Elasped(Start)));
	return 0;
}
