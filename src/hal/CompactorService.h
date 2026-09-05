#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

enum class CompactAlgo {
  LZX = 0,
  XPRESS8K = 1,
};

enum class GameCompactionState {
  Uncompressed = 0,
  Compacted = 1,
  InQueue = 2,
  Compacting = 3,
  Decompacting = 4,
  Failed = 5,
};

enum class CompressibilityRating {
  Unknown = 0,
  High = 1,     // ~25%–50% estimated savings (loose textures, binaries, shaders)
  Moderate = 2, // ~15%–25% estimated savings
  Low = 3,      // <10% savings (already compressed videos/zstd/Oodle archives)
};

struct GameEntry {
  std::wstring id;
  std::wstring title;
  std::wstring installPath;
  std::wstring launcher; // L"Steam", L"Epic", L"Hydra"
  uint64_t uncompressedBytes = 0;
  uint64_t compressedBytes = 0;
  GameCompactionState state = GameCompactionState::Uncompressed;
  CompressibilityRating rating = CompressibilityRating::Unknown;
  std::wstring analysisNote;
  float progress = 0.0f; // 0.0 .. 100.0
  float savingsPercent = 0.0f;
  int retries = 0;
  std::wstring currentFile;
  bool hasDirectStorage = false;
};

class CompactorService {
public:
  static CompactorService &Get();

  enum class ScanStage {
    Idle = 0,
    ScanningSteam,
    ScanningEpic,
    ScanningHydra,
    ScanningCustom,
    MeasuringSizes,
    Complete,
  };

  void StartScan(bool force = false);
  bool IsScanning() const { return m_isScanning.load(); }
  std::wstring GetScanStatusText() const;

  std::vector<GameEntry> GetGames();
  bool IsBusy() const { return m_isBusy.load(); }
  void StartCompact(size_t index, CompactAlgo algo = CompactAlgo::LZX);
  void StartDecompact(size_t index);
  void StartCompactAll(CompactAlgo algo = CompactAlgo::LZX);
  void CancelOperation();

  bool AddCustomFolder(const std::wstring &folderPath);

  CompactAlgo GetSelectedAlgo() const { return m_algo; }
  void SetSelectedAlgo(CompactAlgo algo) { m_algo = algo; }

  uint64_t GetTotalUncompressedBytes();
  uint64_t GetTotalCompressedBytes();
  uint64_t GetTotalReclaimedBytes();

  static void CalculateDirectorySizesRecursive(const std::wstring &dir,
                                              uint64_t &outLogical,
                                              uint64_t &outPhysical);
  static bool CheckUsesDirectStorage(const std::wstring &installPath);

private:
  CompactorService();
  ~CompactorService();

  void ScanWorker();
  void CompactionWorker();
  void ScanSteam(std::vector<GameEntry> &games);
  void ScanEpic(std::vector<GameEntry> &games);
  void ScanHydra(std::vector<GameEntry> &games);
  void ScanCustom(std::vector<GameEntry> &games);
  void MeasureDirectorySizesFast(GameEntry &entry);
  void AnalyzeGameWorthiness(GameEntry &entry);

  bool RunCompactCommand(const std::wstring &path, bool decompact,
                         CompactAlgo algo,
                         uint64_t totalUncompressedBytes,
                         std::function<void(float, const std::wstring &)> onProgress,
                         uint64_t &outUncompressed,
                         uint64_t &outCompressed);

  std::mutex m_mutex;
  std::vector<GameEntry> m_games;
  std::atomic<bool> m_isScanning{false};
  std::atomic<ScanStage> m_scanStage{ScanStage::Idle};
  std::atomic<bool> m_isBusy{false};
  std::atomic<bool> m_cancelRequested{false};
  CompactAlgo m_algo = CompactAlgo::LZX;

  HANDLE m_hCurrentProcess = nullptr;
  std::vector<size_t> m_queue;
};
